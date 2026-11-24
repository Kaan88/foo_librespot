package spotifyx

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"sync"

	librespot "github.com/devgianlu/go-librespot"
	connectpb "github.com/devgianlu/go-librespot/proto/spotify/connectstate"
)

const (
	// recentlyPlayedLimit is how many contexts the Recently played section
	// holds (Spotify's own cap for this service).
	recentlyPlayedLimit = 50
	// recentlyPlayedConcurrency bounds the playlist metadata reads of the
	// Recently played section.
	recentlyPlayedConcurrency = 4
)

// recentlyPlayedReply is the JSON projection of the recently-played service.
// It lists contexts, not tracks: the album, playlist, artist or collection a
// track was played from.
type recentlyPlayedReply struct {
	PlayContexts []struct {
		URI                string `json:"uri"`
		LastPlayedTime     int64  `json:"lastPlayedTime"`
		LastPlayedTrackURI string `json:"lastPlayedTrackUri"`
	} `json:"playContexts"`
}

// recentlyPlayedQuery asks for the JSON projection and all context kinds.
func recentlyPlayedQuery() url.Values {
	return url.Values{
		"format": {"json"},
		"offset": {"0"},
		"limit":  {fmt.Sprint(recentlyPlayedLimit)},
		"filter": {"default,collection-new-episodes"},
	}
}

// parseRecentlyPlayed turns the reply into context URIs, most recent first,
// without duplicates. Kinds the library cannot describe (shows, episodes,
// unknown entries) are skipped.
func parseRecentlyPlayed(body []byte) ([]string, error) {
	var reply recentlyPlayedReply
	if err := json.Unmarshal(body, &reply); err != nil {
		return nil, fmt.Errorf("decoding recently played: %w", err)
	}
	out := make([]string, 0, len(reply.PlayContexts))
	seen := make(map[string]bool, len(reply.PlayContexts))
	for _, c := range reply.PlayContexts {
		uri := c.URI
		if uri == "" || seen[uri] || !describableContext(uri) {
			continue
		}
		seen[uri] = true
		out = append(out, uri)
		if len(out) == recentlyPlayedLimit {
			break
		}
	}
	return out, nil
}

// describableContext reports whether the library can build an item for a
// recently-played context: albums, artists, playlists and Liked Songs.
func describableContext(uri string) bool {
	switch {
	case strings.HasPrefix(uri, "spotify:album:"),
		strings.HasPrefix(uri, "spotify:artist:"),
		strings.HasPrefix(uri, "spotify:playlist:"):
		return true
	case strings.HasPrefix(uri, "spotify:user:") && strings.HasSuffix(uri, ":collection"):
		return true
	}
	return false
}

// fetchRecentlyPlayed reads the recently-played contexts of username.
func fetchRecentlyPlayed(ctx context.Context, env *Env, username string) ([]string, error) {
	resp, err := env.Session.Spclient().Request(ctx, "GET",
		"/recently-played/v3/user/"+url.PathEscape(username)+"/recently-played", recentlyPlayedQuery(), nil, nil)
	if err != nil {
		return nil, err
	}
	defer func() { _ = resp.Body.Close() }()

	data := readBody(resp)
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("recently played answered %d", resp.StatusCode)
	}
	return parseRecentlyPlayed(data)
}

// recentlyPlayedItems describes the contexts in order. A source that cannot
// describe one context leaves it out; a playlist whose metadata read fails is
// still listed by URI, like an album with missing metadata.
func (m *Manager) recentlyPlayedItems(ctx context.Context, env *Env, uris []string, username string) []Item {
	var albumURIs, artistURIs, playlistURIs []string
	for _, uri := range uris {
		switch {
		case strings.HasPrefix(uri, "spotify:album:"):
			albumURIs = append(albumURIs, uri)
		case strings.HasPrefix(uri, "spotify:artist:"):
			artistURIs = append(artistURIs, uri)
		case strings.HasPrefix(uri, "spotify:playlist:"):
			playlistURIs = append(playlistURIs, uri)
		}
	}

	albumByURI := m.albumItems(ctx, env, albumURIs)
	artistByURI := m.artistItems(ctx, env, artistURIs)
	playlistByURI := m.recentPlaylistItems(ctx, env, playlistURIs)

	out := make([]Item, 0, len(uris))
	for _, uri := range uris {
		switch {
		case strings.HasPrefix(uri, "spotify:album:"):
			if item, ok := albumByURI[uri]; ok {
				out = append(out, item)
			}
		case strings.HasPrefix(uri, "spotify:artist:"):
			if item, ok := artistByURI[uri]; ok {
				out = append(out, item)
			}
		case strings.HasPrefix(uri, "spotify:playlist:"):
			if item, ok := playlistByURI[uri]; ok {
				out = append(out, item)
			} else {
				out = append(out, Item{URI: uri, Kind: ItemKindPlaylist})
			}
		case strings.HasPrefix(uri, "spotify:user:") && strings.HasSuffix(uri, ":collection"):
			out = append(out, Item{
				URI:      uri,
				Kind:     ItemKindCollection,
				Name:     "Liked Songs",
				Subtitle: username,
			})
		}
	}
	return out
}

// recentPlaylistItems describes playlists through the playlist service, which
// gives their name, owner, picture and revision in one request each. The reads
// run a few at a time.
func (m *Manager) recentPlaylistItems(ctx context.Context, env *Env, uris []string) map[string]Item {
	out := make(map[string]Item, len(uris))
	if len(uris) == 0 {
		return out
	}

	type result struct {
		uri  string
		item Item
		ok   bool
	}
	results := make([]result, len(uris))
	var (
		wg  sync.WaitGroup
		sem = make(chan struct{}, recentlyPlayedConcurrency)
	)
	for i, uri := range uris {
		if item, ok := m.items.get(uri); ok {
			results[i] = result{uri: uri, item: item, ok: true}
			continue
		}
		wg.Add(1)
		sem <- struct{}{}
		go func(i int, uri string) {
			defer wg.Done()
			defer func() { <-sem }()

			id, err := librespot.SpotifyIdFromUri(uri)
			if err != nil {
				return
			}
			spotCtx, err := env.Session.Spclient().PlaylistContext(ctx, *id)
			if err != nil {
				m.log.WithError(err).Warnf("recently played: reading playlist %s failed", uri)
				return
			}
			item := playlistItemFromContext(uri, spotCtx)
			m.items.put(uri, item)
			results[i] = result{uri: uri, item: item, ok: true}
		}(i, uri)
	}
	wg.Wait()

	for _, r := range results {
		if r.ok {
			out[r.uri] = r.item
		}
	}
	return out
}

// playlistItemFromContext describes a playlist from its context metadata: the
// name, owner, picture and revision the playlist service reports.
func playlistItemFromContext(uri string, spotCtx *connectpb.Context) Item {
	meta := spotCtx.GetMetadata()
	item := Item{
		URI:      uri,
		Kind:     ItemKindPlaylist,
		Name:     meta["context_description"],
		Subtitle: meta["context_owner"],
	}
	if image := meta["image_url"]; image != "" {
		item.ImageURL = stringPtr(image)
	}
	if rev := meta["playlist.revision"]; rev != "" && strings.Trim(rev, "0") != "" {
		item.Revision = stringPtr(rev)
	}
	return item
}
