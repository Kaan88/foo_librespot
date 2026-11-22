package spike

import (
	"fmt"
	"net/url"
	"regexp"
	"strings"

	librespot "github.com/devgianlu/go-librespot"
)

var base62ID = regexp.MustCompile(`^[0-9A-Za-z]{22}$`)

// ParseTrackID accepts a spotify:track: URI, an open.spotify.com track link, or
// a bare 22-character base62 track id and returns the library's id type.
func ParseTrackID(raw string) (librespot.SpotifyId, error) {
	s := strings.TrimSpace(raw)
	if s == "" {
		return librespot.SpotifyId{}, fmt.Errorf("empty track id")
	}

	if strings.HasPrefix(s, "spotify:") {
		id, err := librespot.SpotifyIdFromUri(s)
		if err != nil {
			return librespot.SpotifyId{}, err
		}
		if id.Type() != librespot.SpotifyIdTypeTrack {
			return librespot.SpotifyId{}, fmt.Errorf("not a track uri: %s", s)
		}
		return *id, nil
	}

	if strings.Contains(s, "://") || strings.HasPrefix(s, "open.spotify.com/") {
		u, err := url.Parse(s)
		if err != nil {
			return librespot.SpotifyId{}, fmt.Errorf("parsing track url: %w", err)
		}
		if u.Host != "" && !strings.EqualFold(u.Host, "open.spotify.com") {
			return librespot.SpotifyId{}, fmt.Errorf("unsupported host %q (expected open.spotify.com)", u.Host)
		}

		// /track/<id>, optionally behind /intl-xx/.
		parts := strings.Split(strings.Trim(u.Path, "/"), "/")
		for i, part := range parts {
			if part == "track" && i+1 < len(parts) {
				return spotifyIdFromBase62(parts[i+1])
			}
		}
		return librespot.SpotifyId{}, fmt.Errorf("no /track/<id> segment in %s", s)
	}

	return spotifyIdFromBase62(s)
}

func spotifyIdFromBase62(id string) (librespot.SpotifyId, error) {
	if !base62ID.MatchString(id) {
		return librespot.SpotifyId{}, fmt.Errorf("invalid Spotify track id %q (expected 22 base62 characters)", id)
	}
	parsed, err := librespot.SpotifyIdFromBase62(librespot.SpotifyIdTypeTrack, id)
	if err != nil {
		return librespot.SpotifyId{}, err
	}
	return *parsed, nil
}
