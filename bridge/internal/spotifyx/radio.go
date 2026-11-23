package spotifyx

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strings"

	librespot "github.com/devgianlu/go-librespot"
	connectpb "github.com/devgianlu/go-librespot/proto/spotify/connectstate"
	playerpb "github.com/devgianlu/go-librespot/proto/spotify/player"

	"foo_librespot/bridge/internal/spotifyuri"
)

// maxRadioTracks bounds a radio reply.
const maxRadioTracks = 200

var trackURIPattern = regexp.MustCompile(`^spotify:track:[0-9A-Za-z]{22}$`)

// stationURI is the radio context of a seed: spotify:station:<kind>:<id>.
func stationURI(ref spotifyuri.Ref) (string, error) {
	switch ref.Kind {
	case spotifyuri.KindTrack, spotifyuri.KindArtist, spotifyuri.KindAlbum, spotifyuri.KindPlaylist:
		return "spotify:station:" + string(ref.Kind) + ":" + ref.ID, nil
	}
	return "", badInputf("radio needs a track, artist, album or playlist, not a %s", ref.Kind)
}

// extractTrackURIs collects, in order and without duplicates, every
// spotify:track URI found anywhere in a JSON document. It is deliberately
// shape-agnostic: the radio endpoints answer with different envelopes.
func extractTrackURIs(body []byte) []string {
	var doc any
	if err := json.Unmarshal(body, &doc); err != nil {
		return nil
	}
	var out []string
	seen := make(map[string]bool)
	var walk func(v any)
	walk = func(v any) {
		switch t := v.(type) {
		case string:
			if trackURIPattern.MatchString(t) && !seen[t] {
				seen[t] = true
				out = append(out, t)
			}
		case []any:
			for _, e := range t {
				walk(e)
			}
		case map[string]any:
			// Map iteration is unordered, but the envelopes that matter hold
			// their tracks in one array, which keeps its order.
			for _, e := range t {
				walk(e)
			}
		}
	}
	walk(doc)
	return out
}

// contextTrackURIs lists the track URIs of the inline pages of a context.
func contextTrackURIs(c *connectpb.Context) []string {
	var out []string
	seen := make(map[string]bool)
	for _, page := range c.GetPages() {
		for _, track := range page.GetTracks() {
			if u := track.GetUri(); trackURIPattern.MatchString(u) && !seen[u] {
				seen[u] = true
				out = append(out, u)
			}
		}
	}
	return out
}

// radioURIs finds the tracks of a radio for the seed. It tries, in order, the
// station context resolver, the autoplay resolver and the radio-apollo
// endpoint, and reports which one answered.
func (m *Manager) radioURIs(ctx context.Context, env *Env, ref spotifyuri.Ref, station string) (uris []string, via string, err error) {
	sp := env.Session.Spclient()
	var failures []string

	// 1. The context resolver with a station URI.
	if spotCtx, err := sp.ContextResolve(ctx, station); err != nil {
		failures = append(failures, fmt.Sprintf("context resolve %s: %v", station, err))
	} else if u := contextTrackURIs(spotCtx); len(u) > 0 {
		return capURIs(u), "context-resolve " + station, nil
	} else if all, err := walkContext(ctx, env, m.log, spotCtx, station); err == nil && len(all) > 0 {
		return capURIs(all), "context-resolve pages " + station, nil
	} else {
		failures = append(failures, fmt.Sprintf("context resolve %s: no tracks", station))
	}

	// 2. The autoplay resolver, which Spotify clients use to continue a context.
	areq := &playerpb.AutoplayContextRequest{ContextUri: &ref.URI}
	if ref.Kind == spotifyuri.KindTrack {
		areq.RecentTrackUri = []string{ref.URI}
	}
	if spotCtx, err := sp.ContextResolveAutoplay(ctx, areq); err != nil {
		failures = append(failures, fmt.Sprintf("autoplay: %v", err))
	} else if u := contextTrackURIs(spotCtx); len(u) > 0 {
		return capURIs(u), "context-resolve/v1/autoplay", nil
	} else {
		failures = append(failures, "autoplay: no tracks")
	}

	// 3. radio-apollo, the legacy station endpoint.
	path := "/radio-apollo/v3/stations/" + ref.URI
	query := url.Values{"autoplay": {"true"}, "count": {"50"}}
	resp, rerr := sp.Request(ctx, "GET", path, query, nil, nil)
	if rerr != nil {
		failures = append(failures, fmt.Sprintf("radio-apollo: %v", rerr))
	} else {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
		_ = resp.Body.Close()
		if resp.StatusCode == http.StatusOK {
			if u := extractTrackURIs(body); len(u) > 0 {
				return capURIs(u), "radio-apollo/v3/stations", nil
			}
			failures = append(failures, "radio-apollo: no tracks")
		} else {
			failures = append(failures, fmt.Sprintf("radio-apollo: HTTP %d", resp.StatusCode))
		}
	}

	return nil, "", Unsupportedf("Spotify has no radio for %s reachable from this login (%s)", ref.URI, strings.Join(failures, "; "))
}

func capURIs(u []string) []string {
	if len(u) > maxRadioTracks {
		return u[:maxRadioTracks]
	}
	return u
}

// seedName is the display name of a radio seed.
func (m *Manager) seedName(ctx context.Context, env *Env, ref spotifyuri.Ref) string {
	switch ref.Kind {
	case spotifyuri.KindTrack:
		if metas, err := m.Lookup(ctx, []string{ref.URI}); err == nil && metas[ref.URI] != nil {
			return metas[ref.URI].Track.GetName()
		}
	case spotifyuri.KindAlbum:
		return m.albumItems(ctx, env, []string{ref.URI})[ref.URI].Name
	case spotifyuri.KindArtist:
		return m.artistItems(ctx, env, []string{ref.URI})[ref.URI].Name
	case spotifyuri.KindPlaylist:
		if id, err := librespot.SpotifyIdFromUri(ref.URI); err == nil {
			if c, err := env.Session.Spclient().PlaylistContext(ctx, *id); err == nil {
				return c.GetMetadata()["context_description"]
			}
		}
	}
	return ""
}

// Radio returns the tracks of a radio seeded by a track, artist, album or
// playlist, in the shape of a resolve reply.
func (m *Manager) Radio(ctx context.Context, seed string) ([]TrackInfo, *ContextInfo, error) {
	ref, err := spotifyuri.Parse(seed)
	if err != nil {
		return nil, nil, badInputf("%v", err)
	}
	station, err := stationURI(ref)
	if err != nil {
		return nil, nil, err
	}

	var (
		uris []string
		name string
	)
	err = m.Do(ctx, func(env *Env) error {
		var via string
		var err error
		uris, via, err = m.radioURIs(ctx, env, ref, station)
		if err != nil {
			return err
		}
		m.log.Infof("radio for %s: %d tracks via %s", ref.URI, len(uris), via)
		name = m.seedName(ctx, env, ref)
		return nil
	})
	if err != nil {
		if errors.Is(err, io.EOF) {
			return nil, nil, Unsupportedf("radio for %s is empty", ref.URI)
		}
		return nil, nil, err
	}

	metas, err := m.Lookup(ctx, uris)
	if err != nil {
		return nil, nil, err
	}
	return m.buildInfos(uris, metas, defaultBitrate), radioContext(station, name, uris), nil
}

// radioContext describes a radio as a resolve context.
func radioContext(station, seedName string, uris []string) *ContextInfo {
	name := "Radio"
	if seedName != "" {
		name = seedName + " Radio"
	}
	return &ContextInfo{
		URI:        station,
		Name:       name,
		Owner:      "Spotify",
		Revision:   revisionHash(uris),
		TrackCount: len(uris),
	}
}
