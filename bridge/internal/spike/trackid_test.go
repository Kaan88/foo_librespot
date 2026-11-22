package spike

import "testing"

func TestParseTrackID(t *testing.T) {
	const want = "4uLU6hMCjMI75M1A2tKUQC"

	cases := []string{
		"spotify:track:" + want,
		"https://open.spotify.com/track/" + want,
		"https://open.spotify.com/track/" + want + "?si=abc123",
		"https://open.spotify.com/intl-de/track/" + want,
		"open.spotify.com/track/" + want,
		want,
		"  " + want + "  ",
	}

	for _, tc := range cases {
		id, err := ParseTrackID(tc)
		if err != nil {
			t.Errorf("ParseTrackID(%q): %v", tc, err)
			continue
		}
		if got := id.Base62(); got != want {
			t.Errorf("ParseTrackID(%q).Base62() = %q, want %q", tc, got, want)
		}
		if id.Type() != "track" {
			t.Errorf("ParseTrackID(%q).Type() = %q, want track", tc, id.Type())
		}
	}
}

func TestParseTrackIDErrors(t *testing.T) {
	cases := []string{
		"",
		"spotify:album:4uLU6hMCjMI75M1A2tKUQC",
		"https://example.com/track/4uLU6hMCjMI75M1A2tKUQC",
		"https://open.spotify.com/album/4uLU6hMCjMI75M1A2tKUQC",
		"short",
	}

	for _, tc := range cases {
		if _, err := ParseTrackID(tc); err == nil {
			t.Errorf("ParseTrackID(%q) unexpectedly succeeded", tc)
		}
	}
}
