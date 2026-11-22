package spotifyuri

import "testing"

func TestParseAccepts(t *testing.T) {
	const id = "4uLU6hMCjMI75M1A2tKUQC"

	cases := []struct {
		in   string
		kind Kind
		uri  string
	}{
		{"spotify:track:" + id, KindTrack, "spotify:track:" + id},
		{"spotify:album:" + id, KindAlbum, "spotify:album:" + id},
		{"spotify:playlist:" + id, KindPlaylist, "spotify:playlist:" + id},
		{"https://open.spotify.com/track/" + id, KindTrack, "spotify:track:" + id},
		{"https://open.spotify.com/track/" + id + "?si=abc123", KindTrack, "spotify:track:" + id},
		{"https://open.spotify.com/intl-de/track/" + id, KindTrack, "spotify:track:" + id},
		{"open.spotify.com/album/" + id + "?si=x", KindAlbum, "spotify:album:" + id},
		{"https://open.spotify.com/playlist/" + id, KindPlaylist, "spotify:playlist:" + id},
		{"  spotify:track:" + id + "  ", KindTrack, "spotify:track:" + id},
		{id, KindTrack, "spotify:track:" + id},
		{"spotify:user:someuser:collection", KindLiked, "spotify:user:someuser:collection"},
	}

	for _, tc := range cases {
		ref, err := Parse(tc.in)
		if err != nil {
			t.Errorf("Parse(%q): %v", tc.in, err)
			continue
		}
		if ref.Kind != tc.kind || ref.URI != tc.uri {
			t.Errorf("Parse(%q) = %+v, want kind %q uri %q", tc.in, ref, tc.kind, tc.uri)
		}
	}
}

func TestParseRejects(t *testing.T) {
	cases := []string{
		"",
		"spotify:artist:4uLU6hMCjMI75M1A2tKUQC",
		"spotify:track:short",
		"spotify:track:4uLU6hMCjMI75M1A2tKUQC:extra",
		"spotify:user:someuser:playlists",
		"https://example.com/track/4uLU6hMCjMI75M1A2tKUQC",
		"https://open.spotify.com/show/4uLU6hMCjMI75M1A2tKUQC",
		"https://open.spotify.com/track/short",
		"https://open.spotify.com/",
		"not a track",
	}
	for _, tc := range cases {
		if _, err := Parse(tc); err == nil {
			t.Errorf("Parse(%q) unexpectedly succeeded", tc)
		}
	}
}
