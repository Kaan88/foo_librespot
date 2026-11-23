// Package spotifyuri parses the Spotify URIs and web links the bridge accepts.
package spotifyuri

import (
	"fmt"
	"net/url"
	"regexp"
	"strings"
)

// Kind is what a parsed reference points at.
type Kind string

const (
	KindTrack    Kind = "track"
	KindAlbum    Kind = "album"
	KindArtist   Kind = "artist"
	KindPlaylist Kind = "playlist"
	KindLiked    Kind = "liked"
)

var base62ID = regexp.MustCompile(`^[0-9A-Za-z]{22}$`)

// Ref is a parsed Spotify reference.
type Ref struct {
	Kind Kind
	// URI is the canonical spotify: URI. For KindLiked it keeps the user form
	// (spotify:user:<user>:collection).
	URI string
	// ID is the base62 id of the item (a track, album or playlist).
	ID string
}

// Parse accepts spotify: URIs, open.spotify.com links (optionally behind an
// /intl-xx/ segment and with query parameters) and a bare 22-character track id.
func Parse(raw string) (Ref, error) {
	s := strings.TrimSpace(raw)
	if s == "" {
		return Ref{}, fmt.Errorf("empty spotify reference")
	}

	if strings.HasPrefix(s, "spotify:") {
		return parseURI(s)
	}

	if strings.Contains(s, "://") || strings.HasPrefix(s, "open.spotify.com/") || strings.HasPrefix(s, "play.spotify.com/") {
		return parseURL(s)
	}

	if base62ID.MatchString(s) {
		return Ref{Kind: KindTrack, URI: "spotify:track:" + s, ID: s}, nil
	}

	return Ref{}, fmt.Errorf("unsupported spotify reference %q", raw)
}

func parseURI(s string) (Ref, error) {
	parts := strings.Split(s, ":")
	if len(parts) < 3 || parts[0] != "spotify" {
		return Ref{}, fmt.Errorf("invalid spotify uri %q", s)
	}

	switch parts[1] {
	case "track", "album", "artist", "playlist":
		if len(parts) != 3 {
			return Ref{}, fmt.Errorf("invalid spotify uri %q", s)
		}
		kind := Kind(parts[1])
		if err := checkID(kind, parts[2]); err != nil {
			return Ref{}, err
		}
		return Ref{Kind: kind, URI: "spotify:" + string(kind) + ":" + parts[2], ID: parts[2]}, nil
	case "user":
		// spotify:user:<user>:collection (Liked Songs)
		if len(parts) == 4 && parts[3] == "collection" && parts[2] != "" {
			return Ref{Kind: KindLiked, URI: s, ID: parts[2]}, nil
		}
		return Ref{}, fmt.Errorf("unsupported user uri %q (only spotify:user:<user>:collection)", s)
	}

	return Ref{}, fmt.Errorf("unsupported spotify uri type %q", parts[1])
}

func parseURL(s string) (Ref, error) {
	if !strings.Contains(s, "://") {
		s = "https://" + s
	}
	u, err := url.Parse(s)
	if err != nil {
		return Ref{}, fmt.Errorf("parsing spotify url: %w", err)
	}
	if !strings.EqualFold(u.Host, "open.spotify.com") && !strings.EqualFold(u.Host, "play.spotify.com") {
		return Ref{}, fmt.Errorf("unsupported host %q (expected open.spotify.com)", u.Host)
	}

	segments := strings.Split(strings.Trim(u.Path, "/"), "/")
	// Drop a leading locale segment: /intl-de/track/<id>.
	if len(segments) > 0 && strings.HasPrefix(segments[0], "intl-") {
		segments = segments[1:]
	}
	if len(segments) != 2 {
		return Ref{}, fmt.Errorf("no /<type>/<id> in %q", s)
	}

	kind := Kind(segments[0])
	switch kind {
	case KindTrack, KindAlbum, KindArtist, KindPlaylist:
	default:
		return Ref{}, fmt.Errorf("unsupported spotify link type %q", segments[0])
	}
	if err := checkID(kind, segments[1]); err != nil {
		return Ref{}, err
	}
	return Ref{Kind: kind, URI: "spotify:" + string(kind) + ":" + segments[1], ID: segments[1]}, nil
}

func checkID(kind Kind, id string) error {
	if !base62ID.MatchString(id) {
		return fmt.Errorf("invalid %s id %q (expected 22 base62 characters)", kind, id)
	}
	return nil
}
