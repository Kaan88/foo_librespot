package spotifyx

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"strings"
)

const (
	// searchViewPath is Spotify's internal search service. It is the only
	// search source reachable from this login that lists playlists; the
	// context resolver only answers tracks.
	searchViewPath = "/searchview/km/v4/search/"
	// searchViewPlatform and searchViewClientVersion are required: without a
	// clientVersion the service answers 400.
	searchViewPlatform      = "WebPlayer"
	searchViewClientVersion = "1.0.0"
)

// searchViewReply is the part of a search answer this bridge reads.
type searchViewReply struct {
	Results struct {
		Playlists struct {
			Hits []struct {
				URI            string `json:"uri"`
				Name           string `json:"name"`
				Author         string `json:"author"`
				Image          string `json:"image"`
				FollowersCount int    `json:"followersCount"`
			} `json:"hits"`
		} `json:"playlists"`
	} `json:"results"`
}

// searchViewParams builds the query of a search request.
func searchViewParams(country, username string, limit int) url.Values {
	q := url.Values{
		"entityVersion": {"2"},
		"limit":         {strconv.Itoa(limit)},
		"offset":        {"0"},
		"locale":        {"en"},
		"username":      {username},
		"platform":      {searchViewPlatform},
		"clientVersion": {searchViewClientVersion},
	}
	if country != "" {
		q.Set("country", country)
	}
	return q
}

// parseSearchViewPlaylists turns the playlist hits of a search answer into
// items, in Spotify's order and without duplicates.
func parseSearchViewPlaylists(body []byte, limit int) ([]Item, error) {
	var reply searchViewReply
	if err := json.Unmarshal(body, &reply); err != nil {
		return nil, fmt.Errorf("decoding the playlist search: %w", err)
	}
	hits := reply.Results.Playlists.Hits
	out := make([]Item, 0, len(hits))
	seen := make(map[string]bool, len(hits))
	for _, hit := range hits {
		if !strings.HasPrefix(hit.URI, "spotify:playlist:") || seen[hit.URI] {
			continue
		}
		seen[hit.URI] = true
		item := Item{
			URI:      hit.URI,
			Kind:     ItemKindPlaylist,
			Name:     hit.Name,
			Subtitle: hit.Author,
		}
		if hit.Image != "" {
			item.ImageURL = stringPtr(hit.Image)
		}
		out = append(out, item)
		if limit > 0 && len(out) >= limit {
			break
		}
	}
	return out, nil
}

// searchPlaylistItems asks the search service for playlists. A failure is not
// fatal for a search: it is logged and the playlist list stays empty.
func (m *Manager) searchPlaylistItems(ctx context.Context, env *Env, query string, limit int) []Item {
	resp, err := env.Session.Spclient().Request(ctx, "GET", searchViewPath+url.PathEscape(query),
		searchViewParams(m.Country(), env.Session.Username(), limit), nil, nil)
	if err != nil {
		m.log.WithError(err).Warnf("playlist search failed")
		return []Item{}
	}
	defer func() { _ = resp.Body.Close() }()

	data := readBody(resp)
	if resp.StatusCode != http.StatusOK {
		m.log.Warnf("playlist search answered %d", resp.StatusCode)
		return []Item{}
	}
	items, err := parseSearchViewPlaylists(data, limit)
	if err != nil {
		m.log.WithError(err).Warnf("playlist search failed")
		return []Item{}
	}
	return items
}
