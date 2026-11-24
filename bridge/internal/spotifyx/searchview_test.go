package spotifyx

import "testing"

func TestParseSearchViewPlaylists(t *testing.T) {
	body := []byte(`{"results":{"playlists":{"total":4,"hits":[
		{"uri":"spotify:playlist:37i9dQZF1EIXh8taIVWHER","name":"Nation of Language Mix","author":"Spotify","image":"https://pickasso.spotifycdn.com/image/a","followersCount":0},
		{"uri":"spotify:track:0gUJlYlu4cV90iyV2gTUmr","name":"not a playlist"},
		{"uri":"spotify:playlist:37i9dQZF1EIXh8taIVWHER","name":"duplicate"},
		{"uri":"spotify:playlist:3tBKdwyGpyOYQfEm5FVGRv","name":"Nation Of Language - Setlist 2026","author":"Eddy Méndez","image":"https://o.scdn.co/300/ab","followersCount":325}
	]}}}`)
	items, err := parseSearchViewPlaylists(body, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 2 {
		t.Fatalf("items = %+v", items)
	}
	first := items[0]
	if first.URI != "spotify:playlist:37i9dQZF1EIXh8taIVWHER" || first.Kind != ItemKindPlaylist ||
		first.Name != "Nation of Language Mix" || first.Subtitle != "Spotify" ||
		first.ImageURL == nil || *first.ImageURL != "https://pickasso.spotifycdn.com/image/a" {
		t.Errorf("first item = %+v", first)
	}
	if items[1].Subtitle != "Eddy Méndez" {
		t.Errorf("second item = %+v", items[1])
	}

	capped, err := parseSearchViewPlaylists(body, 1)
	if err != nil || len(capped) != 1 {
		t.Errorf("capped = %v, %v", capped, err)
	}

	if _, err := parseSearchViewPlaylists([]byte("not json"), 10); err == nil {
		t.Error("garbage accepted")
	}
}

func TestSearchViewParams(t *testing.T) {
	q := searchViewParams("SE", "testuser0001", 10)
	if q.Get("entityVersion") != "2" || q.Get("limit") != "10" || q.Get("offset") != "0" ||
		q.Get("username") != "testuser0001" || q.Get("country") != "SE" ||
		q.Get("platform") != searchViewPlatform || q.Get("clientVersion") != searchViewClientVersion {
		t.Errorf("query = %v", q)
	}
	if got := searchViewParams("", "u", 5); got.Has("country") {
		t.Errorf("empty country was sent: %v", got)
	}
}
