package spotifyx

import (
	"context"
	"fmt"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"time"

	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"github.com/devgianlu/go-librespot/spclient"
	"google.golang.org/protobuf/proto"

	"foo_librespot/bridge/internal/spotifyuri"
)

const (
	// maxFollowURIs bounds one follow/unfollow/following request.
	maxFollowURIs = 500
)

// followTarget is one entity follow/unfollow works on. Artists and albums live
// in collection sets, playlists in the user's rootlist.
type followTarget struct {
	URI  string
	Kind spotifyuri.Kind
}

// followTargets validates and canonicalises the URIs of a follow request.
func followTargets(raw []string) ([]followTarget, error) {
	if len(raw) == 0 {
		return nil, badInputf("uris must not be empty")
	}
	if len(raw) > maxFollowURIs {
		return nil, badInputf("at most %d uris per request", maxFollowURIs)
	}
	out := make([]followTarget, len(raw))
	for i, r := range raw {
		ref, err := spotifyuri.Parse(r)
		if err != nil {
			return nil, badInputf("%v", err)
		}
		switch ref.Kind {
		case spotifyuri.KindArtist, spotifyuri.KindAlbum, spotifyuri.KindPlaylist:
		default:
			return nil, badInputf("%q is not an artist, album or playlist", r)
		}
		out[i] = followTarget{URI: ref.URI, Kind: ref.Kind}
	}
	return out, nil
}

// splitFollowTargets groups the targets by where they are stored.
func splitFollowTargets(targets []followTarget) (artists, albums, playlists []followTarget) {
	for _, t := range targets {
		switch t.Kind {
		case spotifyuri.KindArtist:
			artists = append(artists, t)
		case spotifyuri.KindAlbum:
			albums = append(albums, t)
		case spotifyuri.KindPlaylist:
			playlists = append(playlists, t)
		}
	}
	return artists, albums, playlists
}

func targetURIs(targets []followTarget) []string {
	out := make([]string, len(targets))
	for i, t := range targets {
		out[i] = t.URI
	}
	return out
}

// followedFlags assembles the reply of "following" in request order.
func followedFlags(targets []followTarget, artists, albums, rootlist map[string]bool) []bool {
	out := make([]bool, len(targets))
	for i, t := range targets {
		switch t.Kind {
		case spotifyuri.KindArtist:
			out[i] = artists[t.URI]
		case spotifyuri.KindAlbum:
			out[i] = albums[t.URI]
		case spotifyuri.KindPlaylist:
			out[i] = rootlist[t.URI]
		}
	}
	return out
}

// rootlistMembership lists the playlist URIs of a rootlist. Folder markers and
// other non-playlist entries are not included.
func rootlistMembership(rl *playlist4pb.SelectedListContent) map[string]bool {
	out := make(map[string]bool)
	for _, entry := range rl.GetContents().GetItems() {
		uri := entry.GetUri()
		if strings.HasPrefix(uri, "spotify:playlist:") {
			out[uri] = true
		}
	}
	return out
}

// rootlistIndex is the position of uri in the rootlist, or -1. Folder markers
// count as entries, so the index matches what rootlist changes address.
func rootlistIndex(rl *playlist4pb.SelectedListContent, uri string) int {
	for i, entry := range rl.GetContents().GetItems() {
		if entry.GetUri() == uri {
			return i
		}
	}
	return -1
}

// rootlistAddOp adds uris at the top of the rootlist.
func rootlistAddOp(uris []string) *playlist4pb.Op {
	items := make([]*playlist4pb.Item, 0, len(uris))
	for _, uri := range uris {
		items = append(items, &playlist4pb.Item{Uri: proto.String(uri)})
	}
	return &playlist4pb.Op{
		Kind: playlist4pb.Op_ADD.Enum(),
		Add:  &playlist4pb.Add{Items: items, AddFirst: proto.Bool(true)},
	}
}

// removeByIndexOps removes the entries at indices. They are sorted from the
// highest position down, so every op addresses the list state the previous ops
// left behind, whatever order the server applies them in.
func removeByIndexOps(indices []int) []*playlist4pb.Op {
	unique := make(map[int]bool, len(indices))
	sorted := make([]int, 0, len(indices))
	for _, idx := range indices {
		if idx < 0 || unique[idx] {
			continue
		}
		unique[idx] = true
		sorted = append(sorted, idx)
	}
	sort.Sort(sort.Reverse(sort.IntSlice(sorted)))

	ops := make([]*playlist4pb.Op, 0, len(sorted))
	for _, idx := range sorted {
		ops = append(ops, &playlist4pb.Op{
			Kind: playlist4pb.Op_REM.Enum(),
			Rem:  &playlist4pb.Rem{FromIndex: proto.Int32(int32(idx)), Length: proto.Int32(1)},
		})
	}
	return ops
}

// rootlistChangesRequest builds one rootlist change against revision.
func rootlistChangesRequest(username string, revision []byte, ops []*playlist4pb.Op, now time.Time) (*PlaylistRequest, error) {
	msg := &playlist4pb.ListChanges{
		BaseRevision: revision,
		Deltas: []*playlist4pb.Delta{{
			Ops: ops,
			Info: &playlist4pb.ChangeInfo{
				User:      proto.String(username),
				Timestamp: proto.Int64(now.UnixMilli()),
			},
		}},
	}
	return previewMessage("POST", "/playlist/v2/user/"+url.PathEscape(username)+"/rootlist/changes", msg, "the rootlist change")
}

// rootlistChange reads the current rootlist, lets build name the operations and
// sends them (or only returns the request when dryRun is set). A change is not
// idempotent: it is sent once, retried once on 409 with a freshly read
// rootlist. build returns no ops when there is nothing to do; the reply is then
// nil.
func rootlistChange(ctx context.Context, env *Env, username string, dryRun bool,
	build func(*playlist4pb.SelectedListContent) ([]*playlist4pb.Op, error)) (*PlaylistRequest, error) {
	sp := env.Session.Spclient()
	for attempt := 0; ; attempt++ {
		rootlist, err := fetchRootlist(ctx, env, username)
		if err != nil {
			return nil, fmt.Errorf("reading the playlist library: %w", err)
		}
		ops, err := build(rootlist)
		if err != nil {
			return nil, err
		}
		if len(ops) == 0 {
			return nil, nil
		}
		req, err := rootlistChangesRequest(username, rootlist.GetRevision(), ops, time.Now())
		if err != nil {
			return nil, err
		}
		if dryRun {
			return req, nil
		}

		resp, err := sendOnce(ctx, sp, req, nil)
		if err != nil {
			return nil, err
		}
		data := readBody(resp)
		_ = resp.Body.Close()
		switch {
		case resp.StatusCode == http.StatusOK:
			return req, nil
		case resp.StatusCode == http.StatusConflict && attempt == 0:
			continue
		default:
			return nil, writeFailure("the library change", resp.StatusCode, data)
		}
	}
}

// collectionContains answers, in order, whether uris are in a collection set.
func collectionContains(ctx context.Context, sp *spclient.Spclient, username, set string, uris []string) ([]bool, error) {
	body, err := buildCollectionContains(username, set, uris)
	if err != nil {
		return nil, err
	}
	resp, err := sp.Request(ctx, "POST", "/collection/v2/contains", nil, collectionHeader(), body)
	if err != nil {
		return nil, err
	}
	defer func() { _ = resp.Body.Close() }()
	data := readBody(resp)
	if resp.StatusCode != http.StatusOK {
		if isClientStatus(resp.StatusCode) {
			return nil, rejected("the library check", resp.StatusCode, data)
		}
		return nil, fmt.Errorf("collection contains answered %d", resp.StatusCode)
	}
	return parseContains(data, len(uris))
}

// Following tells for each artist, album or playlist URI whether the user
// follows it (artists and albums: in the collection sets, playlists: in the
// rootlist).
func (m *Manager) Following(ctx context.Context, rawURIs []string) ([]bool, error) {
	targets, err := followTargets(rawURIs)
	if err != nil {
		return nil, err
	}
	artists, albums, playlists := splitFollowTargets(targets)

	var flags []bool
	err = m.Do(ctx, func(env *Env) error {
		sp := env.Session.Spclient()
		username := env.Session.Username()

		artistSet := make(map[string]bool)
		if len(artists) > 0 {
			found, err := collectionContains(ctx, sp, username, spclient.CollectionSetArtists, targetURIs(artists))
			if err != nil {
				return err
			}
			for i, t := range artists {
				artistSet[t.URI] = found[i]
			}
		}

		albumSet := make(map[string]bool)
		if len(albums) > 0 {
			found, err := collectionContains(ctx, sp, username, spclient.CollectionSetLikedSongs, targetURIs(albums))
			if err != nil {
				return err
			}
			for i, t := range albums {
				albumSet[t.URI] = found[i]
			}
		}

		rootlist := make(map[string]bool)
		if len(playlists) > 0 {
			rl, err := fetchRootlist(ctx, env, username)
			if err != nil {
				return fmt.Errorf("reading the playlist library: %w", err)
			}
			rootlist = rootlistMembership(rl)
		}

		flags = followedFlags(targets, artistSet, albumSet, rootlist)
		return nil
	})
	return flags, err
}

// SetFollow follows (adds artists/albums to the collection sets and playlists
// to the top of the rootlist) or unfollows its URIs. With dryRun it reads what
// it needs and returns the requests without sending them.
func (m *Manager) SetFollow(ctx context.Context, rawURIs []string, follow, dryRun bool) ([]*PlaylistRequest, error) {
	targets, err := followTargets(rawURIs)
	if err != nil {
		return nil, err
	}
	artists, albums, playlists := splitFollowTargets(targets)

	var requests []*PlaylistRequest
	err = m.Do(ctx, func(env *Env) error {
		sp := env.Session.Spclient()
		username := env.Session.Username()

		for _, group := range []struct {
			what    string
			set     string
			targets []followTarget
		}{
			{"the artist follow", spclient.CollectionSetArtists, artists},
			{"the album save", spclient.CollectionSetLikedSongs, albums},
		} {
			if len(group.targets) == 0 {
				continue
			}
			msg := collectionWriteMessage(username, group.set, targetURIs(group.targets), !follow, time.Now())
			req, err := previewMessage("POST", "/collection/v2/write", msg, group.what)
			if err != nil {
				return err
			}
			if dryRun {
				requests = append(requests, req)
				continue
			}
			resp, err := sendOnce(ctx, sp, req, collectionHeader())
			if err != nil {
				return err
			}
			data := readBody(resp)
			_ = resp.Body.Close()
			if resp.StatusCode != http.StatusOK {
				return writeFailure("the collection change", resp.StatusCode, data)
			}
		}

		if len(playlists) == 0 {
			return nil
		}
		req, err := rootlistChange(ctx, env, username, dryRun, func(rl *playlist4pb.SelectedListContent) ([]*playlist4pb.Op, error) {
			if follow {
				var add []string
				for _, t := range playlists {
					if rootlistIndex(rl, t.URI) < 0 {
						add = append(add, t.URI)
					}
				}
				if len(add) == 0 {
					return nil, nil
				}
				return []*playlist4pb.Op{rootlistAddOp(add)}, nil
			}
			var indices []int
			for _, t := range playlists {
				if idx := rootlistIndex(rl, t.URI); idx >= 0 {
					indices = append(indices, idx)
				}
			}
			return removeByIndexOps(indices), nil
		})
		if err != nil {
			return err
		}
		if req != nil && dryRun {
			requests = append(requests, req)
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	if !dryRun {
		m.invalidateLibrary()
	}
	return requests, nil
}
