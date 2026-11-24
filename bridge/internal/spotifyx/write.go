package spotifyx

import (
	"bytes"
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	librespot "github.com/devgianlu/go-librespot"
	collectionpb "github.com/devgianlu/go-librespot/proto/spotify/collection/v2"
	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"github.com/devgianlu/go-librespot/spclient"
	"google.golang.org/protobuf/encoding/prototext"
	"google.golang.org/protobuf/proto"

	"foo_librespot/bridge/internal/spotifyuri"
)

const (
	collectionContentType = "application/vnd.collection-v2.spotify.proto"
	// maxWriteURIs bounds one saved/save/unsave/playlist_add request.
	maxWriteURIs = 500
	// maxErrorBody bounds how much of Spotify's answer an error carries.
	maxErrorBody = 300
)

// ErrBadInput is returned for requests the bridge refuses before sending them
// (or that Spotify refuses as not permitted); the server maps it to
// bad_request.
type ErrBadInput struct{ Msg string }

func (e *ErrBadInput) Error() string { return e.Msg }

// IsBadInput reports whether err is an ErrBadInput.
func IsBadInput(err error) bool {
	var e *ErrBadInput
	return errors.As(err, &e)
}

// finalError marks a failure that must not trigger a session recreation and an
// automatic resend (a write whose outcome is unknown).
type finalError struct{ error }

func (e *finalError) Unwrap() error { return e.error }

func badInputf(format string, args ...any) error {
	return &ErrBadInput{Msg: fmt.Sprintf(format, args...)}
}

// trackURIs validates and canonicalises a list of track URIs.
func trackURIs(raw []string) ([]string, error) {
	if len(raw) == 0 {
		return nil, badInputf("uris must not be empty")
	}
	if len(raw) > maxWriteURIs {
		return nil, badInputf("at most %d uris per request", maxWriteURIs)
	}
	out := make([]string, len(raw))
	for i, r := range raw {
		ref, err := spotifyuri.Parse(r)
		if err != nil || ref.Kind != spotifyuri.KindTrack {
			return nil, badInputf("%q is not a track", r)
		}
		out[i] = ref.URI
	}
	return out, nil
}

// buildCollectionContains builds the body of /collection/v2/contains for a
// collection set (Liked Songs/tracks, albums or followed artists).
func buildCollectionContains(username, set string, uris []string) ([]byte, error) {
	return proto.Marshal(collectionContainsMessage(username, set, uris))
}

func collectionContainsMessage(username, set string, uris []string) *collectionpb.ContainsRequest {
	req := &collectionpb.ContainsRequest{Username: username, Set: set}
	for _, uri := range uris {
		req.Items = append(req.Items, &collectionpb.CollectionItem{Uri: uri})
	}
	return req
}

// buildCollectionWrite builds the body of /collection/v2/write for a collection
// set.
func buildCollectionWrite(username, set string, uris []string, remove bool, now time.Time) ([]byte, error) {
	return proto.Marshal(collectionWriteMessage(username, set, uris, remove, now))
}

func collectionWriteMessage(username, set string, uris []string, remove bool, now time.Time) *collectionpb.WriteRequest {
	req := &collectionpb.WriteRequest{Username: username, Set: set}
	for _, uri := range uris {
		item := &collectionpb.CollectionItem{Uri: uri, IsRemoved: remove}
		if !remove {
			item.AddedAt = now.Unix()
		}
		req.Items = append(req.Items, item)
	}
	return req
}

// parseContains decodes a /collection/v2/contains answer.
func parseContains(body []byte, want int) ([]bool, error) {
	var resp collectionpb.ContainsResponse
	if err := proto.Unmarshal(body, &resp); err != nil {
		return nil, fmt.Errorf("decoding collection contains: %w", err)
	}
	if len(resp.GetFound()) != want {
		return nil, fmt.Errorf("collection contains answered %d items for %d", len(resp.GetFound()), want)
	}
	return resp.GetFound(), nil
}

// rejected turns a refusal of a write into an UnsupportedError carrying what
// Spotify said.
func rejected(what string, status int, body []byte) error {
	msg := trimBody(body)
	if msg == "" {
		return Unsupportedf("Spotify rejected %s (HTTP %d)", what, status)
	}
	return Unsupportedf("Spotify rejected %s (HTTP %d): %s", what, status, msg)
}

func trimBody(b []byte) string {
	s := strings.TrimSpace(string(bytes.ToValidUTF8(b, nil)))
	if len(s) > maxErrorBody {
		s = s[:maxErrorBody] + "..."
	}
	return s
}

func readBody(resp *http.Response) []byte {
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	return body
}

func collectionHeader() http.Header {
	return http.Header{"Content-Type": {collectionContentType}, "Accept": {collectionContentType}}
}

// Saved tells for each track whether it is in Liked Songs.
func (m *Manager) Saved(ctx context.Context, rawURIs []string) ([]bool, error) {
	uris, err := trackURIs(rawURIs)
	if err != nil {
		return nil, err
	}
	var found []bool
	err = m.Do(ctx, func(env *Env) error {
		body, err := buildCollectionContains(env.Session.Username(), spclient.CollectionSetLikedSongs, uris)
		if err != nil {
			return err
		}
		resp, err := env.Session.Spclient().Request(ctx, "POST", "/collection/v2/contains", nil, collectionHeader(), body)
		if err != nil {
			return err
		}
		defer func() { _ = resp.Body.Close() }()
		data := readBody(resp)
		if resp.StatusCode != http.StatusOK {
			if isClientStatus(resp.StatusCode) {
				return rejected("the saved-tracks check", resp.StatusCode, data)
			}
			return fmt.Errorf("collection contains answered %d", resp.StatusCode)
		}
		found, err = parseContains(data, len(uris))
		return err
	})
	return found, err
}

// SetSaved adds the tracks to (save) or removes them from (!save) Liked Songs,
// then drops the cached library so the next read sees the change.
func (m *Manager) SetSaved(ctx context.Context, rawURIs []string, save bool) error {
	uris, err := trackURIs(rawURIs)
	if err != nil {
		return err
	}
	err = m.Do(ctx, func(env *Env) error {
		body, err := buildCollectionWrite(env.Session.Username(), spclient.CollectionSetLikedSongs, uris, !save, time.Now())
		if err != nil {
			return err
		}
		resp, err := env.Session.Spclient().Request(ctx, "POST", "/collection/v2/write", nil, collectionHeader(), body)
		if err != nil {
			return err
		}
		defer func() { _ = resp.Body.Close() }()
		data := readBody(resp)
		if resp.StatusCode != http.StatusOK {
			if isClientStatus(resp.StatusCode) {
				return rejected("the Liked Songs change", resp.StatusCode, data)
			}
			return fmt.Errorf("collection write answered %d", resp.StatusCode)
		}
		return nil
	})
	if err != nil {
		return err
	}
	m.invalidateLibrary()
	return nil
}

// invalidateLibrary drops the cached library reply. The Liked Songs revision is
// a hash of the freshly read collection, so it changes with the next read.
func (m *Manager) invalidateLibrary() {
	m.libMu.Lock()
	defer m.libMu.Unlock()
	m.libSections = nil
	m.libAt = time.Time{}
}

// PlaylistRequest is a write request as it is (or would be) sent: a playlist
// or rootlist change, a collection write or a playlist creation.
type PlaylistRequest struct {
	Method string `json:"method"`
	Path   string `json:"path"`
	// BodyText is the protobuf message in text form.
	BodyText string `json:"body_text"`
	BodyLen  int    `json:"body_len"`
	// Body is the exact wire body.
	Body []byte `json:"-"`
}

// buildPlaylistAppend builds the playlist v2 changes request that appends uris
// to the end of a playlist, based on revision.
func buildPlaylistAppend(playlistID, username string, revision []byte, uris []string, now time.Time) (*PlaylistRequest, error) {
	items := make([]*playlist4pb.Item, 0, len(uris))
	for _, uri := range uris {
		items = append(items, &playlist4pb.Item{Uri: proto.String(uri)})
	}
	msg := &playlist4pb.ListChanges{
		BaseRevision: revision,
		Deltas: []*playlist4pb.Delta{{
			Ops: []*playlist4pb.Op{{
				Kind: playlist4pb.Op_ADD.Enum(),
				Add:  &playlist4pb.Add{Items: items, AddLast: proto.Bool(true)},
			}},
			Info: &playlist4pb.ChangeInfo{
				User:      proto.String(username),
				Timestamp: proto.Int64(now.UnixMilli()),
			},
		}},
	}
	body, err := proto.Marshal(msg)
	if err != nil {
		return nil, fmt.Errorf("encoding the playlist change: %w", err)
	}
	return &PlaylistRequest{
		Method:   "POST",
		Path:     "/playlist/v2/playlist/" + playlistID + "/changes",
		BodyText: prototext.MarshalOptions{Multiline: false}.Format(msg),
		BodyLen:  len(body),
		Body:     body,
	}, nil
}

// PlaylistChangeResult is the outcome of a playlist change or creation.
type PlaylistChangeResult struct {
	// Revision is the playlist's (or rootlist's) revision after the change
	// (hex); for a dry run it is the revision the change is based on.
	Revision string
	// Request is set for a dry run of a change with one request.
	Request *PlaylistRequest
	// Requests is set for a dry run that needs more than one request (a
	// playlist creation has a rootlist entry, a follow touches several
	// collection sets).
	Requests []*PlaylistRequest
}

// PlaylistAdd appends tracks to a playlist. With dryRun it only reads the
// playlist's current revision and returns the request it would send.
func (m *Manager) PlaylistAdd(ctx context.Context, playlist string, rawURIs []string, dryRun bool) (*PlaylistChangeResult, error) {
	ref, err := spotifyuri.Parse(playlist)
	if err != nil || ref.Kind != spotifyuri.KindPlaylist {
		return nil, badInputf("%q is not a playlist", playlist)
	}
	uris, err := trackURIs(rawURIs)
	if err != nil {
		return nil, err
	}
	id, err := librespot.SpotifyIdFromUri(ref.URI)
	if err != nil {
		return nil, badInputf("%v", err)
	}

	var result *PlaylistChangeResult
	err = m.Do(ctx, func(env *Env) error {
		sp := env.Session.Spclient()
		username := env.Session.Username()

		// One retry when the playlist moved between reading its revision and
		// writing (409).
		for attempt := 0; ; attempt++ {
			revision, err := sp.PlaylistRevision(ctx, *id)
			if err != nil {
				return playlistError("reading the playlist", err)
			}
			req, err := buildPlaylistAppend(ref.ID, username, revision, uris, time.Now())
			if err != nil {
				return err
			}
			if dryRun {
				result = &PlaylistChangeResult{Revision: hex.EncodeToString(revision), Request: req}
				return nil
			}

			// A change is not idempotent: never resend it blindly.
			resp, err := sendOnce(ctx, sp, req, nil)
			if err != nil {
				return err
			}
			data := readBody(resp)
			_ = resp.Body.Close()

			switch {
			case resp.StatusCode == http.StatusOK:
				newRev, err := sp.PlaylistRevision(ctx, *id)
				if err != nil {
					// The tracks are added: never let the manager resend them.
					return &finalError{playlistError("reading the new revision", err)}
				}
				result = &PlaylistChangeResult{Revision: hex.EncodeToString(newRev)}
				return nil
			case resp.StatusCode == http.StatusConflict && attempt == 0:
				continue
			case resp.StatusCode == http.StatusForbidden || resp.StatusCode == http.StatusNotFound:
				return badInputf("cannot add to this playlist (HTTP %d): %s", resp.StatusCode, trimBody(data))
			case isClientStatus(resp.StatusCode):
				return rejected("the playlist change", resp.StatusCode, data)
			default:
				return &finalError{fmt.Errorf("playlist changes answered %d", resp.StatusCode)}
			}
		}
	})
	if err != nil {
		return nil, err
	}
	if !dryRun {
		m.invalidateLibrary()
	}
	return result, nil
}

// playlistError maps a refusal while reading a playlist to a protocol error.
func playlistError(what string, err error) error {
	var se *spclient.StatusError
	if errors.As(err, &se) && (se.StatusCode == http.StatusForbidden || se.StatusCode == http.StatusNotFound) {
		return badInputf("%s failed: playlist not accessible (HTTP %d)", what, se.StatusCode)
	}
	return err
}

// previewMessage builds a dry-run description of one protobuf write request.
func previewMessage(method, path string, msg proto.Message, what string) (*PlaylistRequest, error) {
	body, err := proto.Marshal(msg)
	if err != nil {
		return nil, fmt.Errorf("encoding %s: %w", what, err)
	}
	return &PlaylistRequest{
		Method:   method,
		Path:     path,
		BodyText: prototext.MarshalOptions{Multiline: false}.Format(msg),
		BodyLen:  len(body),
		Body:     body,
	}, nil
}

// writeFailure maps a refused or failed write to a protocol error: 403/404 is
// bad input (not permitted, or no such entity), another 4xx is Spotify's
// refusal, and anything else is an unknown outcome that must not be retried
// automatically.
func writeFailure(what string, status int, body []byte) error {
	switch {
	case status == http.StatusForbidden || status == http.StatusNotFound:
		return badInputf("%s is not permitted (HTTP %d): %s", what, status, trimBody(body))
	case isClientStatus(status):
		return rejected(what, status, body)
	default:
		return &finalError{fmt.Errorf("%s answered %d", what, status)}
	}
}

// sendOnce sends a non-idempotent change exactly once. A transport failure may
// mean Spotify applied it, so it is wrapped as final: the manager must not
// recreate the session and send the change again.
func sendOnce(ctx context.Context, sp *spclient.Spclient, req *PlaylistRequest, header http.Header) (*http.Response, error) {
	resp, err := sp.RequestOnce(ctx, req.Method, req.Path, nil, header, req.Body)
	if err != nil {
		return nil, &finalError{err}
	}
	return resp, nil
}

// readRevision reads a playlist's current revision, mapping an inaccessible
// playlist to bad input.
func readRevision(ctx context.Context, sp *spclient.Spclient, id librespot.SpotifyId) ([]byte, error) {
	revision, err := sp.PlaylistRevision(ctx, id)
	if err != nil {
		return nil, playlistError("reading the playlist", err)
	}
	return revision, nil
}
