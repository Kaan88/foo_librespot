package spotifyx

import (
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"net/http"
	"sort"
	"strings"
	"time"

	librespot "github.com/devgianlu/go-librespot"
	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"github.com/devgianlu/go-librespot/spclient"
	"google.golang.org/protobuf/proto"

	"foo_librespot/bridge/internal/spotifyuri"
)

const (
	// maxPlaylistNameBytes bounds a new playlist name.
	maxPlaylistNameBytes = 1000
	// maxPlaylistRemovals bounds one playlist_remove request.
	maxPlaylistRemovals = 500
)

// PlaylistItemRef is one item to remove from a playlist: the track URI the
// client saw at that position.
type PlaylistItemRef struct {
	URI   string
	Index int
}

// PlaylistCreateResult is the outcome of PlaylistCreate.
type PlaylistCreateResult struct {
	// Playlist is the new item (set when a playlist was created).
	Playlist Item
	// Revision is the new playlist's revision (hex).
	Revision string
	// Requests is set for a dry run (the create request; the rootlist entry can
	// only be built once Spotify assigned the playlist id).
	Requests []*PlaylistRequest
}

// playlistName validates and trims a playlist name.
func playlistName(raw string) (string, error) {
	name := strings.TrimSpace(raw)
	if name == "" {
		return "", badInputf("name must not be empty")
	}
	if len(name) > maxPlaylistNameBytes {
		return "", badInputf("name must be at most %d bytes", maxPlaylistNameBytes)
	}
	return name, nil
}

// playlistRef parses a playlist URI and returns its base62 id.
func playlistRef(raw string) (librespot.SpotifyId, string, error) {
	ref, err := spotifyuri.Parse(raw)
	if err != nil {
		return librespot.SpotifyId{}, "", badInputf("%v", err)
	}
	if ref.Kind != spotifyuri.KindPlaylist {
		return librespot.SpotifyId{}, "", badInputf("%q is not a playlist", raw)
	}
	id, err := librespot.SpotifyIdFromUri(ref.URI)
	if err != nil {
		return librespot.SpotifyId{}, "", badInputf("%v", err)
	}
	return *id, ref.ID, nil
}

// playlistItemRefs validates the items of a playlist_remove request.
func playlistItemRefs(raw []PlaylistItemRef) ([]PlaylistItemRef, error) {
	if len(raw) == 0 {
		return nil, badInputf("items must not be empty")
	}
	if len(raw) > maxPlaylistRemovals {
		return nil, badInputf("at most %d items per request", maxPlaylistRemovals)
	}
	out := make([]PlaylistItemRef, len(raw))
	seen := make(map[int]bool, len(raw))
	for i, item := range raw {
		ref, err := spotifyuri.Parse(item.URI)
		if err != nil || ref.Kind != spotifyuri.KindTrack {
			return nil, badInputf("%q is not a track", item.URI)
		}
		if item.Index < 0 {
			return nil, badInputf("items[%d].index must not be negative", i)
		}
		if seen[item.Index] {
			return nil, badInputf("items[%d].index %d is given twice", i, item.Index)
		}
		seen[item.Index] = true
		out[i] = PlaylistItemRef{URI: ref.URI, Index: item.Index}
	}
	return out, nil
}

// validateRemoval checks every item against the playlist's current contents
// and returns the positions to remove, highest first. An item that sits at its
// index is taken as is. Otherwise, when its URI appears exactly once in the
// playlist, that position is used (a foobar2000 copy of a playlist leaves out
// entries it cannot play, such as episodes, so its positions can be lower than
// Spotify's). Anything else is refused rather than remove the wrong track.
func validateRemoval(contents []string, items []PlaylistItemRef) ([]int, error) {
	positions := make(map[string][]int)
	for i, uri := range contents {
		positions[uri] = append(positions[uri], i)
	}
	taken := make(map[int]bool, len(items))
	indices := make([]int, 0, len(items))
	for _, item := range items {
		idx := -1
		switch {
		case item.Index < len(contents) && contents[item.Index] == item.URI:
			idx = item.Index
		case len(positions[item.URI]) == 1:
			idx = positions[item.URI][0]
		case len(positions[item.URI]) == 0:
			return nil, badInputf("%q is not in the playlist any more", item.URI)
		default:
			return nil, badInputf("%q is in the playlist %d times and not at index %d; the playlist changed",
				item.URI, len(positions[item.URI]), item.Index)
		}
		if taken[idx] {
			return nil, badInputf("%q at index %d is given twice", item.URI, idx)
		}
		taken[idx] = true
		indices = append(indices, idx)
	}
	sort.Sort(sort.Reverse(sort.IntSlice(indices)))
	return indices, nil
}

// buildPlaylistCreate builds POST /playlist/v2/playlist carrying the name.
func buildPlaylistCreate(name, username string, now time.Time) (*PlaylistRequest, error) {
	msg := &playlist4pb.ListUpdateRequest{
		Attributes: &playlist4pb.ListAttributes{Name: proto.String(name)},
		Info: &playlist4pb.ChangeInfo{
			User:      proto.String(username),
			Timestamp: proto.Int64(now.UnixMilli()),
		},
	}
	return previewMessage("POST", "/playlist/v2/playlist", msg, "the playlist creation")
}

// parseCreateListReply reads the new playlist's URI and revision.
func parseCreateListReply(body []byte) (string, []byte, error) {
	var reply playlist4pb.CreateListReply
	if err := proto.Unmarshal(body, &reply); err != nil {
		return "", nil, fmt.Errorf("decoding the new playlist: %w", err)
	}
	ref, err := spotifyuri.Parse(reply.GetUri())
	if err != nil || ref.Kind != spotifyuri.KindPlaylist {
		return "", nil, fmt.Errorf("Spotify answered a playlist creation with %q", reply.GetUri())
	}
	return ref.URI, reply.GetRevision(), nil
}

// buildPlaylistRename builds a change that renames a playlist, based on
// revision.
func buildPlaylistRename(playlistID, username, name string, revision []byte, now time.Time) (*PlaylistRequest, error) {
	msg := &playlist4pb.ListChanges{
		BaseRevision: revision,
		Deltas: []*playlist4pb.Delta{{
			Ops: []*playlist4pb.Op{{
				Kind: playlist4pb.Op_UPDATE_LIST_ATTRIBUTES.Enum(),
				UpdateListAttributes: &playlist4pb.UpdateListAttributes{
					NewAttributes: &playlist4pb.ListAttributesPartialState{
						Values: &playlist4pb.ListAttributes{Name: proto.String(name)},
					},
				},
			}},
			Info: &playlist4pb.ChangeInfo{
				User:      proto.String(username),
				Timestamp: proto.Int64(now.UnixMilli()),
			},
		}},
	}
	return previewMessage("POST", "/playlist/v2/playlist/"+playlistID+"/changes", msg, "the playlist change")
}

// buildPlaylistRemove builds a change that removes the items at indices (the
// caller validated them against the current contents), based on revision.
func buildPlaylistRemove(playlistID, username string, revision []byte, indices []int, now time.Time) (*PlaylistRequest, error) {
	msg := &playlist4pb.ListChanges{
		BaseRevision: revision,
		Deltas: []*playlist4pb.Delta{{
			Ops: removeByIndexOps(indices),
			Info: &playlist4pb.ChangeInfo{
				User:      proto.String(username),
				Timestamp: proto.Int64(now.UnixMilli()),
			},
		}},
	}
	return previewMessage("POST", "/playlist/v2/playlist/"+playlistID+"/changes", msg, "the playlist change")
}

// PlaylistCreate creates an empty playlist and adds it to the top of the
// library. With dryRun it only returns the create request.
func (m *Manager) PlaylistCreate(ctx context.Context, rawName string, dryRun bool) (*PlaylistCreateResult, error) {
	name, err := playlistName(rawName)
	if err != nil {
		return nil, err
	}

	var result *PlaylistCreateResult
	err = m.Do(ctx, func(env *Env) error {
		sp := env.Session.Spclient()
		username := env.Session.Username()

		req, err := buildPlaylistCreate(name, username, time.Now())
		if err != nil {
			return err
		}
		if dryRun {
			result = &PlaylistCreateResult{Requests: []*PlaylistRequest{req}}
			return nil
		}

		resp, err := sendOnce(ctx, sp, req, nil)
		if err != nil {
			return err
		}
		data := readBody(resp)
		_ = resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			return writeFailure("the playlist creation", resp.StatusCode, data)
		}
		uri, revision, err := parseCreateListReply(data)
		if err != nil {
			return err
		}

		// A new playlist belongs at the top of the library. Spotify may have
		// added it already; then there is nothing to do.
		if _, err := rootlistChange(ctx, env, username, false,
			func(rl *playlist4pb.SelectedListContent) ([]*playlist4pb.Op, error) {
				if rootlistIndex(rl, uri) >= 0 {
					return nil, nil
				}
				return []*playlist4pb.Op{rootlistAddOp([]string{uri})}, nil
			}); err != nil {
			return &finalError{fmt.Errorf("created playlist %s but adding it to the library failed: %w", uri, err)}
		}

		item := playlistItem(uri, name, username, "", 0, itemRevision(revision))
		editable := true
		item.Editable = &editable
		result = &PlaylistCreateResult{
			Playlist: item,
			Revision: hex.EncodeToString(revision),
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	if !dryRun {
		m.invalidateLibrary()
	}
	return result, nil
}

// PlaylistRename changes a playlist's name.
func (m *Manager) PlaylistRename(ctx context.Context, playlist, rawName string, dryRun bool) (*PlaylistChangeResult, error) {
	id, base62, err := playlistRef(playlist)
	if err != nil {
		return nil, err
	}
	name, err := playlistName(rawName)
	if err != nil {
		return nil, err
	}

	var result *PlaylistChangeResult
	err = m.Do(ctx, func(env *Env) error {
		sp := env.Session.Spclient()
		username := env.Session.Username()

		for attempt := 0; ; attempt++ {
			revision, err := readRevision(ctx, sp, id)
			if err != nil {
				return err
			}
			req, err := buildPlaylistRename(base62, username, name, revision, time.Now())
			if err != nil {
				return err
			}
			if dryRun {
				result = &PlaylistChangeResult{Revision: hex.EncodeToString(revision), Request: req}
				return nil
			}

			resp, err := sendOnce(ctx, sp, req, nil)
			if err != nil {
				return err
			}
			data := readBody(resp)
			_ = resp.Body.Close()
			switch {
			case resp.StatusCode == http.StatusOK:
				// The change is applied: a failure from here on must not
				// make the manager send it again.
				newRev, err := readRevision(ctx, sp, id)
				if err != nil {
					return &finalError{fmt.Errorf("the change was applied, but reading the new revision failed: %w", err)}
				}
				result = &PlaylistChangeResult{Revision: hex.EncodeToString(newRev)}
				return nil
			case resp.StatusCode == http.StatusConflict && attempt == 0:
				continue
			default:
				return writeFailure("the playlist change", resp.StatusCode, data)
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

// PlaylistDelete removes a playlist from the user's library (Spotify has no
// delete; the playlist itself stays, unfollowed).
func (m *Manager) PlaylistDelete(ctx context.Context, playlist string, dryRun bool) (*PlaylistChangeResult, error) {
	_, base62, err := playlistRef(playlist)
	if err != nil {
		return nil, err
	}
	uri := "spotify:playlist:" + base62

	var result *PlaylistChangeResult
	err = m.Do(ctx, func(env *Env) error {
		req, err := rootlistChange(ctx, env, env.Session.Username(), dryRun,
			func(rl *playlist4pb.SelectedListContent) ([]*playlist4pb.Op, error) {
				idx := rootlistIndex(rl, uri)
				if idx < 0 {
					return nil, nil
				}
				return removeByIndexOps([]int{idx}), nil
			})
		if err != nil {
			return err
		}
		result = &PlaylistChangeResult{Request: req}
		return nil
	})
	if err != nil {
		return nil, err
	}
	if !dryRun {
		m.invalidateLibrary()
	}
	return result, nil
}

// PlaylistRemove removes items from a playlist. Every item must still sit at
// its index in the current revision: a moved playlist answers bad_request
// instead of removing a different track. A 409 is retried once with the fresh
// contents.
func (m *Manager) PlaylistRemove(ctx context.Context, playlist string, rawItems []PlaylistItemRef, dryRun bool) (*PlaylistChangeResult, error) {
	id, base62, err := playlistRef(playlist)
	if err != nil {
		return nil, err
	}
	items, err := playlistItemRefs(rawItems)
	if err != nil {
		return nil, err
	}

	var result *PlaylistChangeResult
	err = m.Do(ctx, func(env *Env) error {
		sp := env.Session.Spclient()
		username := env.Session.Username()

		for attempt := 0; ; attempt++ {
			revision, contents, err := sp.PlaylistContents(ctx, id)
			if err != nil {
				if errors.Is(err, spclient.ErrPlaylistConflict) && attempt == 0 {
					continue
				}
				return playlistError("reading the playlist", err)
			}
			indices, err := validateRemoval(contents, items)
			if err != nil {
				return err
			}
			req, err := buildPlaylistRemove(base62, username, revision, indices, time.Now())
			if err != nil {
				return err
			}
			if dryRun {
				result = &PlaylistChangeResult{Revision: hex.EncodeToString(revision), Request: req}
				return nil
			}

			resp, err := sendOnce(ctx, sp, req, nil)
			if err != nil {
				return err
			}
			data := readBody(resp)
			_ = resp.Body.Close()
			switch {
			case resp.StatusCode == http.StatusOK:
				// The change is applied: a failure from here on must not
				// make the manager send it again.
				newRev, err := readRevision(ctx, sp, id)
				if err != nil {
					return &finalError{fmt.Errorf("the change was applied, but reading the new revision failed: %w", err)}
				}
				result = &PlaylistChangeResult{Revision: hex.EncodeToString(newRev)}
				return nil
			case resp.StatusCode == http.StatusConflict && attempt == 0:
				continue
			default:
				return writeFailure("the playlist change", resp.StatusCode, data)
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
