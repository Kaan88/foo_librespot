package spotifyx

import (
	"strings"
	"testing"

	connectpb "github.com/devgianlu/go-librespot/proto/spotify/connectstate"
)

func TestParseRecentlyPlayed(t *testing.T) {
	body := []byte(`{"playContexts":[
		{"uri":"spotify:album:3ESvHsZEscT58FNYE92nk6","lastPlayedTime":1,"lastPlayedTrackUri":"spotify:track:x"},
		{"uri":"spotify:playlist:37i9dQZF1E4llIDQeWS2hh","lastPlayedTime":2,"lastPlayedTrackUri":"spotify:track:y"},
		{"uri":"spotify:artist:5JWBow4ywgKNQ5HBxY8hcz"},
		{"uri":"spotify:user:u:collection"},
		{"uri":"spotify:episode:1VDxaxECCJJh4of5htFnmK"},
		{"uri":"spotify:album:3ESvHsZEscT58FNYE92nk6"},
		{"uri":""}
	]}`)
	got, err := parseRecentlyPlayed(body)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		"spotify:album:3ESvHsZEscT58FNYE92nk6",
		"spotify:playlist:37i9dQZF1E4llIDQeWS2hh",
		"spotify:artist:5JWBow4ywgKNQ5HBxY8hcz",
		"spotify:user:u:collection",
	}
	if len(got) != len(want) {
		t.Fatalf("uris = %v", got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("uri %d = %q, want %q", i, got[i], want[i])
		}
	}

	if _, err := parseRecentlyPlayed([]byte("not json")); err == nil {
		t.Error("garbage accepted")
	}
	if got, err := parseRecentlyPlayed([]byte(`{}`)); err != nil || len(got) != 0 {
		t.Errorf("empty reply = %v, %v", got, err)
	}
}

func TestRecentlyPlayedQuery(t *testing.T) {
	q := recentlyPlayedQuery()
	if q.Get("format") != "json" || q.Get("limit") != "50" || q.Get("offset") != "0" ||
		!strings.Contains(q.Get("filter"), "collection-new-episodes") {
		t.Errorf("query = %v", q)
	}
}

func TestDescribableContext(t *testing.T) {
	yes := []string{"spotify:album:x", "spotify:artist:x", "spotify:playlist:x", "spotify:user:u:collection"}
	no := []string{"spotify:show:x", "spotify:episode:x", "spotify:track:x", "spotify:user:u:playlist:x", ""}
	for _, uri := range yes {
		if !describableContext(uri) {
			t.Errorf("%q should be describable", uri)
		}
	}
	for _, uri := range no {
		if describableContext(uri) {
			t.Errorf("%q should be skipped", uri)
		}
	}
}

func TestPlaylistItemFromContext(t *testing.T) {
	ctx := &connectpb.Context{Metadata: map[string]string{
		"context_description": "Nation of Language Mix",
		"context_owner":       "Spotify",
		"image_url":           "https://i.scdn.co/image/abc",
		"playlist.revision":   "00000000c276921a",
	}}
	item := playlistItemFromContext(pA, ctx)
	if item.URI != pA || item.Kind != ItemKindPlaylist || item.Name != "Nation of Language Mix" ||
		item.Subtitle != "Spotify" || item.ImageURL == nil || *item.ImageURL != "https://i.scdn.co/image/abc" {
		t.Errorf("item = %+v", item)
	}
	if item.Revision == nil || *item.Revision != "00000000c276921a" {
		t.Errorf("revision = %v", item.Revision)
	}

	zero := playlistItemFromContext(pB, &connectpb.Context{Metadata: map[string]string{
		"context_description": "Radio",
		"playlist.revision":   "0000000000000000",
	}})
	if zero.Revision != nil {
		t.Errorf("zero revision = %q, want nil", *zero.Revision)
	}
	if zero.ImageURL != nil {
		t.Errorf("missing image = %q, want nil", *zero.ImageURL)
	}
}
