package spotifyx

import (
	"testing"

	connectpb "github.com/devgianlu/go-librespot/proto/spotify/connectstate"

	"foo_librespot/bridge/internal/spotifyuri"
)

func TestStationURI(t *testing.T) {
	ref, _ := spotifyuri.Parse(tA)
	got, err := stationURI(ref)
	if err != nil || got != "spotify:station:track:0VjIjW4GlUZAMYd2vXMi3b" {
		t.Errorf("stationURI = %q, %v", got, err)
	}
	ref, _ = spotifyuri.Parse("spotify:artist:5JWBow4ywgKNQ5HBxY8hcz")
	if got, _ := stationURI(ref); got != "spotify:station:artist:5JWBow4ywgKNQ5HBxY8hcz" {
		t.Errorf("artist station = %q", got)
	}
	liked, _ := spotifyuri.Parse("spotify:user:me:collection")
	if _, err := stationURI(liked); !IsBadInput(err) {
		t.Errorf("liked radio err = %v", err)
	}
}

func TestExtractTrackURIs(t *testing.T) {
	body := []byte(`{"tracks":[{"uri":"` + tA + `","x":["spotify:album:1VDxaxECCJJh4of5htFnmK"]},{"uri":"` + tB + `"},{"uri":"` + tA + `"}]}`)
	got := extractTrackURIs(body)
	if len(got) != 2 || got[0] != tA || got[1] != tB {
		t.Errorf("uris = %v", got)
	}
	if extractTrackURIs([]byte("not json")) != nil {
		t.Error("garbage produced uris")
	}
}

func TestContextTrackURIs(t *testing.T) {
	c := &connectpb.Context{Pages: []*connectpb.ContextPage{{Tracks: []*connectpb.ContextTrack{
		{Uri: tA}, {Uri: "spotify:episode:1VDxaxECCJJh4of5htFnmK"}, {Uri: tB}, {Uri: tA},
	}}}}
	got := contextTrackURIs(c)
	if len(got) != 2 || got[0] != tA || got[1] != tB {
		t.Errorf("uris = %v", got)
	}
}

func TestRadioContext(t *testing.T) {
	c := radioContext("spotify:station:track:x", "Blinding Lights", []string{tA})
	if c.Name != "Blinding Lights Radio" || c.URI != "spotify:station:track:x" || c.TrackCount != 1 || c.Revision == "" {
		t.Errorf("context = %+v", c)
	}
	if radioContext("s", "", nil).Name != "Radio" {
		t.Error("unnamed seed")
	}
}
