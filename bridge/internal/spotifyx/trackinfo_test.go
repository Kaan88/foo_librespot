package spotifyx

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	audiofilespb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata/audiofiles"
	metadatapb "github.com/devgianlu/go-librespot/proto/spotify/metadata"
)

type fakeDecoder struct {
	lastGranulepos int64
}

func (f *fakeDecoder) Read([]float32) (int, error) { return 0, nil }
func (f *fakeDecoder) SetPositionMs(int64) error   { return nil }
func (f *fakeDecoder) PositionMs() int64           { return 0 }

type fakeDecoderNoGranule struct{}

func (f *fakeDecoderNoGranule) Read([]float32) (int, error) { return 0, nil }
func (f *fakeDecoderNoGranule) SetPositionMs(int64) error   { return nil }
func (f *fakeDecoderNoGranule) PositionMs() int64           { return 0 }

func TestGranuleReader(t *testing.T) {
	g := newGranuleReader(&fakeDecoder{lastGranulepos: 2640192})
	if v, ok := g(); !ok || v != 2640192 {
		t.Fatalf("granule = %d, %v; want 2640192, true", v, ok)
	}

	if _, ok := newGranuleReader(&fakeDecoderNoGranule{})(); ok {
		t.Fatal("decoder without a granule reported one")
	}
}

func TestReplayGain(t *testing.T) {
	gain, peak := replayGain(&audiofilespb.NormalizationParams{LoudnessDb: -8.356323, TruePeakDb: 0.8627129})
	if gain == nil || peak == nil {
		t.Fatal("nil gains")
	}
	if math.Abs(*gain-(-18+8.356323)) > 1e-5 {
		t.Errorf("gain = %v, want %v", *gain, -18.0-(-8.356323))
	}
	wantPeak := math.Pow(10, 0.8627129/20)
	if math.Abs(*peak-wantPeak) > 1e-5 {
		t.Errorf("peak = %v, want %v", *peak, wantPeak)
	}

	if g, p := replayGain(nil); g != nil || p != nil {
		t.Error("nil params produced gains")
	}
}

func TestBuildTrackInfo(t *testing.T) {
	track := &metadatapb.Track{
		Name:       strptr("Blinding Lights"),
		Artist:     []*metadatapb.Artist{{Name: strptr("The Weeknd")}},
		Number:     i32ptr(9),
		DiscNumber: i32ptr(1),
		Duration:   i32ptr(200040),
		Album: &metadatapb.Album{
			Name:   strptr("After Hours"),
			Artist: []*metadatapb.Artist{{Name: strptr("The Weeknd")}},
			Date:   &metadatapb.Date{Year: i32ptr(2020), Month: i32ptr(3), Day: i32ptr(20)},
			CoverGroup: &metadatapb.ImageGroup{Image: []*metadatapb.Image{
				{FileId: make([]byte, 20), Width: i32ptr(64), Height: i32ptr(64)},
				{FileId: []byte{0xab, 0xcd}, Width: i32ptr(640), Height: i32ptr(640)},
			}},
		},
	}
	meta := &trackMeta{
		Track: track,
		Audio: &audiofilespb.AudioFilesExtensionResponse{
			Files: []*audiofilespb.ExtendedAudioFile{
				{File: &metadatapb.AudioFile{Format: metadatapb.AudioFile_OGG_VORBIS_160.Enum()}},
				{File: &metadatapb.AudioFile{Format: metadatapb.AudioFile_OGG_VORBIS_320.Enum()}},
			},
			DefaultFileNormalizationParams: &audiofilespb.NormalizationParams{LoudnessDb: -8.356323, TruePeakDb: 0.8627129},
		},
	}

	info := BuildTrackInfo("spotify:track:0VjIjW4GlUZAMYd2vXMi3b", meta, 320)
	if info.URI != "spotify:track:0VjIjW4GlUZAMYd2vXMi3b" {
		t.Errorf("uri = %q", info.URI)
	}
	if info.Title != "Blinding Lights" || len(info.Artists) != 1 || info.Artists[0] != "The Weeknd" {
		t.Errorf("title/artists = %q %v", info.Title, info.Artists)
	}
	if info.Album != "After Hours" || info.AlbumArtist != "The Weeknd" || info.Date != "2020" {
		t.Errorf("album = %q / %q / %q", info.Album, info.AlbumArtist, info.Date)
	}
	if info.TrackNumber != 9 || info.DiscNumber != 1 || info.DurationMs != 200040 {
		t.Errorf("numbers = %d %d %d", info.TrackNumber, info.DiscNumber, info.DurationMs)
	}
	if info.Codec != "vorbis" || info.Bitrate != 320 || !info.Playable {
		t.Errorf("format = %q %d playable=%v", info.Codec, info.Bitrate, info.Playable)
	}
	if info.CoverURL != "https://i.scdn.co/image/abcd" {
		t.Errorf("cover = %q", info.CoverURL)
	}
	if info.TrackGainDB == nil || info.TrackPeak == nil {
		t.Error("missing track gain")
	}
	if info.AlbumGainDB != nil || info.AlbumPeak != nil {
		t.Error("album gain without album params")
	}
}

func TestBuildTrackInfoUnknown(t *testing.T) {
	info := BuildTrackInfo("spotify:track:x", nil, 320)
	if info.Playable || info.Artists == nil || info.URI != "spotify:track:x" {
		t.Errorf("info = %+v", info)
	}
}

func TestBuildTrackInfoURIs(t *testing.T) {
	gid := []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
	agid := []byte{16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1}
	meta := &trackMeta{Track: &metadatapb.Track{
		Name:   strptr("x"),
		Artist: []*metadatapb.Artist{{Name: strptr("A"), Gid: agid}, {Name: strptr("B")}},
		Album:  &metadatapb.Album{Name: strptr("Al"), Gid: gid},
	}}
	info := BuildTrackInfo("spotify:track:t", meta, 320)
	if info.AlbumURI != albumURI(gid) || info.AlbumURI == "" {
		t.Errorf("album_uri = %q", info.AlbumURI)
	}
	if len(info.ArtistURIs) != 1 || info.ArtistURIs[0] != artistURI(agid) {
		t.Errorf("artist_uris = %v", info.ArtistURIs)
	}
	raw, _ := json.Marshal(info)
	if !strings.Contains(string(raw), `"album_uri":"spotify:album:`) || !strings.Contains(string(raw), `"artist_uris":["spotify:artist:`) {
		t.Errorf("json = %s", raw)
	}
	raw, _ = json.Marshal(BuildTrackInfo("spotify:track:u", nil, 320))
	if strings.Contains(string(raw), "album_uri") || strings.Contains(string(raw), "artist_uris") {
		t.Errorf("unknown track should omit the optional fields: %s", raw)
	}
}

func strptr(s string) *string { return &s }
func i32ptr(v int32) *int32   { return &v }
