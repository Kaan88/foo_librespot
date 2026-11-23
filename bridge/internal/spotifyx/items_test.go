package spotifyx

import (
	"testing"

	librespot "github.com/devgianlu/go-librespot"
	metadatapb "github.com/devgianlu/go-librespot/proto/spotify/metadata"
	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"github.com/devgianlu/go-librespot/proto/spotify/playlist_permission"
)

func TestPlaylistEditable(t *testing.T) {
	yes, no := true, false
	tests := []struct {
		name string
		meta *playlist4pb.MetaItem
		want bool
	}{
		{"owner", &playlist4pb.MetaItem{OwnerUsername: strptr("Me")}, true},
		{"collaborative", &playlist4pb.MetaItem{OwnerUsername: strptr("x"), Attributes: &playlist4pb.ListAttributes{Collaborative: &yes}}, true},
		{"capability", &playlist4pb.MetaItem{OwnerUsername: strptr("x"), Capabilities: &playlist_permission.Capabilities{CanEditItems: &yes}}, true},
		{"capability off", &playlist4pb.MetaItem{OwnerUsername: strptr("x"), Capabilities: &playlist_permission.Capabilities{CanEditItems: &no}}, false},
		{"other", &playlist4pb.MetaItem{OwnerUsername: strptr("x")}, false},
		{"no owner info", &playlist4pb.MetaItem{}, false},
	}
	for _, tc := range tests {
		if got := playlistEditable(tc.meta, "me"); got != tc.want {
			t.Errorf("%s: editable = %v, want %v", tc.name, got, tc.want)
		}
	}
}

func TestRevisionHash(t *testing.T) {
	a := revisionHash([]string{"spotify:track:a", "spotify:track:b"})
	b := revisionHash([]string{"spotify:track:a", "spotify:track:b"})
	if a != b {
		t.Fatalf("same list hashed differently: %q vs %q", a, b)
	}
	if len(a) != 32 {
		t.Errorf("hash = %q, want 32 hex characters", a)
	}

	reordered := revisionHash([]string{"spotify:track:b", "spotify:track:a"})
	if reordered == a {
		t.Error("a reordered list kept the same revision")
	}
	if revisionHash(nil) == a {
		t.Error("an empty list kept the same revision")
	}
}

func TestContextRevision(t *testing.T) {
	uris := []string{"spotify:track:a", "spotify:track:b"}
	if got := contextRevision("6b00df00", uris); got != "6b00df00" {
		t.Errorf("revision = %q, want the playlist revision", got)
	}
	if got := contextRevision("", uris); got != revisionHash(uris) {
		t.Errorf("empty revision = %q, want the content hash", got)
	}
	if got := contextRevision("0000000000", uris); got != revisionHash(uris) {
		t.Errorf("zero revision = %q, want the content hash", got)
	}
}

func TestItemRevision(t *testing.T) {
	if got := itemRevision([]byte{0x6b, 0x00, 0xdf, 0x00}); got == nil || *got != "6b00df00" {
		t.Errorf("revision = %v, want 6b00df00", got)
	}
	if got := itemRevision([]byte{0, 0, 0, 0}); got != nil {
		t.Errorf("zero revision = %q, want nil", *got)
	}
	if got := itemRevision(nil); got != nil {
		t.Errorf("missing revision = %q, want nil", *got)
	}
}

// rootlistContent builds a decorated rootlist with a folder marker among the
// playlists, as Spotify returns it.
func rootlistContent() *playlist4pb.SelectedListContent {
	return &playlist4pb.SelectedListContent{
		Contents: &playlist4pb.ListItems{
			Items: []*playlist4pb.Item{
				{Uri: strptr("spotify:playlist:37i9dQZF1E8M5Z6GDxnHFg")},
				{Uri: strptr("spotify:start-group:folder")},
				{Uri: strptr("spotify:playlist:7xFpG0TMUI9eU6JBo1mt5S")},
				{Uri: strptr("spotify:playlist:5rf2TXhUcBxBdkvliKzCFx")},
			},
			MetaItems: []*playlist4pb.MetaItem{
				{
					Revision:      []byte{0, 0, 0, 0},
					Length:        i32ptr(50),
					OwnerUsername: strptr("spotify"),
					Attributes: &playlist4pb.ListAttributes{
						Name: strptr("On Division St Radio"),
						FormatAttributes: []*playlist4pb.FormatListAttribute{
							{Key: strptr("image_url"), Value: strptr("https://pickasso.spotifycdn.com/image/radio")},
						},
					},
				},
				{Attributes: &playlist4pb.ListAttributes{Name: strptr("folder")}},
				{
					Revision:      []byte{0, 0, 0, 3},
					Length:        i32ptr(6),
					OwnerUsername: strptr("testuser0001"),
					Attributes:    &playlist4pb.ListAttributes{Name: strptr("sare")},
				},
				{
					Revision:      []byte{0, 0, 0, 0x16},
					Length:        i32ptr(16),
					OwnerUsername: strptr("rockstargames"),
					Attributes: &playlist4pb.ListAttributes{
						Name:    strptr("Wave 103 (GTA: VC)"),
						Picture: []byte{0xab, 0xcd},
					},
				},
			},
		},
	}
}

func TestSplitRootlist(t *testing.T) {
	madeFor, playlists := splitRootlist(rootlistContent(), "testuser0001")

	if len(madeFor) != 1 {
		t.Fatalf("made for you = %d items, want 1: %+v", len(madeFor), madeFor)
	}
	radio := madeFor[0]
	if radio.URI != "spotify:playlist:37i9dQZF1E8M5Z6GDxnHFg" || radio.Kind != ItemKindPlaylist {
		t.Errorf("radio item = %+v", radio)
	}
	if radio.Name != "On Division St Radio" || radio.Subtitle != "spotify" {
		t.Errorf("radio name/owner = %q / %q", radio.Name, radio.Subtitle)
	}
	if radio.ImageURL == nil || *radio.ImageURL != "https://pickasso.spotifycdn.com/image/radio" {
		t.Errorf("radio image = %v", radio.ImageURL)
	}
	if radio.TrackCount == nil || *radio.TrackCount != 50 {
		t.Errorf("radio count = %v", radio.TrackCount)
	}
	if radio.Revision != nil {
		t.Errorf("zero revision should be nil, got %q", *radio.Revision)
	}

	// The folder marker is not a playlist and must not appear.
	if len(playlists) != 2 {
		t.Fatalf("playlists = %d items, want 2: %+v", len(playlists), playlists)
	}
	sare := playlists[0]
	if sare.Name != "sare" || *sare.TrackCount != 6 || sare.Revision == nil || *sare.Revision != "00000003" {
		t.Errorf("sare item = %+v", sare)
	}
	if sare.ImageURL != nil {
		t.Errorf("sare image = %v, want nil", *sare.ImageURL)
	}
	if sare.Editable == nil || !*sare.Editable {
		t.Errorf("own playlist should be editable: %v", sare.Editable)
	}
	wave := playlists[1]
	if wave.Editable == nil || *wave.Editable {
		t.Errorf("another user's playlist should not be editable: %v", wave.Editable)
	}
	if wave.ImageURL == nil || *wave.ImageURL != "https://u.scdn.co/images/pl/default/abcd" {
		t.Errorf("wave image = %v", wave.ImageURL)
	}
}

func TestSplitRootlistEmpty(t *testing.T) {
	madeFor, playlists := splitRootlist(nil, "tester")
	if madeFor == nil || playlists == nil || len(madeFor) != 0 || len(playlists) != 0 {
		t.Errorf("empty rootlist = %v / %v, want empty non-nil slices", madeFor, playlists)
	}
}

func TestLikedItem(t *testing.T) {
	uris := []string{"spotify:track:a", "spotify:track:b"}
	item := likedItem("tester", uris)
	if item.URI != "spotify:user:tester:collection" || item.Kind != ItemKindCollection || item.Name != "Liked Songs" {
		t.Errorf("liked item = %+v", item)
	}
	if item.TrackCount == nil || *item.TrackCount != 2 {
		t.Errorf("count = %v", item.TrackCount)
	}
	if item.Revision == nil || *item.Revision != revisionHash(uris) {
		t.Errorf("revision = %v", item.Revision)
	}
}

func TestAlbumAndArtistItems(t *testing.T) {
	album := &metadatapb.Album{
		Name:   strptr("Strange Disciple"),
		Artist: []*metadatapb.Artist{{Name: strptr("Nation of Language")}},
		CoverGroup: &metadatapb.ImageGroup{Image: []*metadatapb.Image{
			{FileId: []byte{0x01}, Width: i32ptr(64), Height: i32ptr(64)},
			{FileId: []byte{0xab, 0xcd}, Width: i32ptr(640), Height: i32ptr(640)},
		}},
	}
	item := albumItem("spotify:album:1VDxaxECCJJh4of5htFnmK", album)
	if item.Kind != ItemKindAlbum || item.Name != "Strange Disciple" || item.Subtitle != "Nation of Language" {
		t.Errorf("album item = %+v", item)
	}
	if item.ImageURL == nil || *item.ImageURL != "https://i.scdn.co/image/abcd" {
		t.Errorf("album image = %v", item.ImageURL)
	}
	if item.TrackCount != nil || item.Revision != nil {
		t.Errorf("album count/revision = %v / %v, want nil", item.TrackCount, item.Revision)
	}

	missing := albumItem("spotify:album:x", nil)
	if missing.URI != "spotify:album:x" || missing.Name != "" {
		t.Errorf("missing album = %+v", missing)
	}

	artist := &metadatapb.Artist{
		Name: strptr("Nation of Language"),
		PortraitGroup: &metadatapb.ImageGroup{Image: []*metadatapb.Image{
			{FileId: []byte{0xef, 0x01}, Width: i32ptr(320), Height: i32ptr(320)},
		}},
	}
	artistItem := artistItem("spotify:artist:5JWBow4ywgKNQ5HBxY8hcz", artist)
	if artistItem.Kind != ItemKindArtist || artistItem.Name != "Nation of Language" {
		t.Errorf("artist item = %+v", artistItem)
	}
	if artistItem.ImageURL == nil || *artistItem.ImageURL != "https://i.scdn.co/image/ef01" {
		t.Errorf("artist image = %v", artistItem.ImageURL)
	}
}

func TestRelatedURIs(t *testing.T) {
	artistGid := []byte{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10}
	albumGid := []byte{0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
		0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20}

	metas := map[string]*trackMeta{
		"spotify:track:a": {Track: &metadatapb.Track{
			Album:  &metadatapb.Album{Gid: albumGid},
			Artist: []*metadatapb.Artist{{Gid: artistGid}},
		}},
		"spotify:track:b": {Track: &metadatapb.Track{
			Album:  &metadatapb.Album{Gid: albumGid},
			Artist: []*metadatapb.Artist{{Gid: artistGid}, {Gid: []byte{0xff}}},
		}},
		"spotify:track:c": nil,
	}
	albums, artists := relatedURIs([]string{"spotify:track:a", "spotify:track:c", "spotify:track:b"}, metas)

	wantAlbum := "spotify:album:" + librespot.GidToBase62(albumGid)
	wantArtist := "spotify:artist:" + librespot.GidToBase62(artistGid)
	if len(albums) != 1 || albums[0] != wantAlbum {
		t.Errorf("albums = %v, want [%s]", albums, wantAlbum)
	}
	if len(artists) != 2 || artists[0] != wantArtist {
		t.Errorf("artists = %v, want [%s <other>]", artists, wantArtist)
	}
}

func TestTopTrackURIs(t *testing.T) {
	tr := []byte{0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
		0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30}
	other := []byte{0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
		0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40}
	artist := &metadatapb.Artist{TopTrack: []*metadatapb.TopTracks{
		{Country: strptr("US"), Track: []*metadatapb.Track{{Gid: other}}},
		{Country: strptr("TR"), Track: []*metadatapb.Track{{Gid: tr}}},
	}}

	uris := topTrackURIs(artist, "TR")
	if len(uris) != 1 || uris[0] != "spotify:track:"+librespot.GidToBase62(tr) {
		t.Errorf("TR top tracks = %v", uris)
	}

	// An unknown country falls back to the first list.
	uris = topTrackURIs(artist, "XX")
	if len(uris) != 1 || uris[0] != "spotify:track:"+librespot.GidToBase62(other) {
		t.Errorf("fallback top tracks = %v", uris)
	}
	if uris := topTrackURIs(&metadatapb.Artist{}, "TR"); len(uris) != 0 {
		t.Errorf("artist without top tracks = %v", uris)
	}
}

func TestAlbumGroupURIs(t *testing.T) {
	gidA := []byte{0x01}
	gidB := []byte{0x02}
	groups := []*metadatapb.AlbumGroup{
		{Album: []*metadatapb.Album{{Gid: gidA}}},
		{Album: []*metadatapb.Album{{Gid: gidA}, {Gid: gidB}}},
		{Album: []*metadatapb.Album{{}}},
	}
	uris := albumGroupURIs(groups)
	if len(uris) != 2 {
		t.Fatalf("uris = %v, want 2 entries", uris)
	}
	if uris[0] != "spotify:album:"+librespot.GidToBase62(gidA) || uris[1] != "spotify:album:"+librespot.GidToBase62(gidB) {
		t.Errorf("uris = %v", uris)
	}
}
