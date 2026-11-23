package spotifyx

import (
	"fmt"
	"math"

	"github.com/devgianlu/go-librespot/player"
	audiofilespb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata/audiofiles"
	metadatapb "github.com/devgianlu/go-librespot/proto/spotify/metadata"
)

// TrackInfo is one element of the protocol's <track_info>.
type TrackInfo struct {
	URI         string   `json:"uri"`
	Title       string   `json:"title"`
	Artists     []string `json:"artists"`
	Album       string   `json:"album"`
	AlbumArtist string   `json:"album_artist"`
	TrackNumber int32    `json:"track_number"`
	DiscNumber  int32    `json:"disc_number"`
	Date        string   `json:"date"`
	DurationMs  int32    `json:"duration_ms"`
	Codec       string   `json:"codec"`
	Bitrate     int      `json:"bitrate"`
	CoverURL    string   `json:"cover_url"`
	TrackGainDB *float64 `json:"track_gain_db"`
	TrackPeak   *float64 `json:"track_peak"`
	AlbumGainDB *float64 `json:"album_gain_db"`
	AlbumPeak   *float64 `json:"album_peak"`
	Playable    bool     `json:"playable"`
	// AlbumURI and ArtistURIs are optional (v3): the album and the artists of
	// the track, for "open album/artist".
	AlbumURI   string   `json:"album_uri,omitempty"`
	ArtistURIs []string `json:"artist_uris,omitempty"`
}

// trackMeta is what a batched metadata request knows about one track.
type trackMeta struct {
	Track *metadatapb.Track
	Audio *audiofilespb.AudioFilesExtensionResponse
}

// trackInfoKey identifies a cached track_info: the same track described for two
// preferred bitrates has two entries.
type trackInfoKey struct {
	uri     string
	bitrate int
}

// BuildTrackInfo converts Spotify metadata into the protocol shape. uri is the
// URI the caller asked for, which may differ from the track actually resolved
// when Spotify relinked a restricted track to an alternative.
func BuildTrackInfo(uri string, meta *trackMeta, preferredBitrate int) TrackInfo {
	info := TrackInfo{URI: uri, Artists: []string{}, Playable: false}
	if meta == nil || meta.Track == nil {
		return info
	}
	t := meta.Track

	info.Title = t.GetName()
	for _, a := range t.GetArtist() {
		if name := a.GetName(); name != "" {
			info.Artists = append(info.Artists, name)
		}
		if uri := artistURI(a.GetGid()); uri != "" {
			info.ArtistURIs = append(info.ArtistURIs, uri)
		}
	}
	if album := t.GetAlbum(); album != nil {
		info.Album = album.GetName()
		info.AlbumURI = albumURI(album.GetGid())
		if artists := album.GetArtist(); len(artists) > 0 {
			info.AlbumArtist = artists[0].GetName()
		}
		info.CoverURL = coverURL(album)
		if date := album.GetDate(); date != nil && date.GetYear() > 0 {
			info.Date = fmt.Sprintf("%04d", date.GetYear())
		}
	}
	info.TrackNumber = t.GetNumber()
	info.DiscNumber = t.GetDiscNumber()
	info.DurationMs = t.GetDuration()

	if f := bestFile(trackFiles(meta), preferredBitrate); f != nil {
		info.Codec = player.GetFormatCodec(f.GetFormat())
		info.Bitrate = player.GetFormatBitrate(f.GetFormat())
		info.Playable = isDecodable(f.GetFormat())
	}

	if meta.Audio != nil {
		if g, p := replayGain(meta.Audio.GetDefaultFileNormalizationParams()); g != nil {
			info.TrackGainDB, info.TrackPeak = g, p
		}
		if g, p := replayGain(meta.Audio.GetDefaultAlbumNormalizationParams()); g != nil {
			info.AlbumGainDB, info.AlbumPeak = g, p
		}
	}

	return info
}

// trackFiles flattens the audio files of either metadata source, preferring the
// audio-files extension because it lists every available format.
func trackFiles(meta *trackMeta) []*metadatapb.AudioFile {
	if meta.Audio != nil {
		files := make([]*metadatapb.AudioFile, 0, len(meta.Audio.GetFiles()))
		for _, f := range meta.Audio.GetFiles() {
			if f.GetFile() != nil {
				files = append(files, f.GetFile())
			}
		}
		if len(files) > 0 {
			return files
		}
	}
	if meta.Track != nil {
		return meta.Track.GetFile()
	}
	return nil
}

// bestFile mirrors the format selection librespot's player does: the format
// closest to the preferred bitrate, with FLAC only when asked for.
func bestFile(files []*metadatapb.AudioFile, preferred int) *metadatapb.AudioFile {
	var best *metadatapb.AudioFile
	bestDist := 0
	for _, f := range files {
		if f == nil || f.Format == nil {
			continue
		}
		if !isDecodable(f.GetFormat()) {
			continue
		}
		dist := player.GetFormatBitrate(f.GetFormat()) - preferred
		if dist < 0 {
			dist = -dist
		}
		if best == nil || dist < bestDist {
			best, bestDist = f, dist
		}
	}
	return best
}

func isDecodable(format metadatapb.AudioFile_Format) bool {
	switch player.GetAudioFileFormatAudioFormat(format) {
	case player.AudioFormatOGGVorbis, player.AudioFormatMP3, player.AudioFormatFLAC:
		return true
	default:
		return false
	}
}

// replayGain converts Spotify's loudness metadata to ReplayGain 2.0 values.
func replayGain(p *audiofilespb.NormalizationParams) (*float64, *float64) {
	if p == nil {
		return nil, nil
	}
	gain := -18 - float64(p.GetLoudnessDb())
	peak := math.Pow(10, float64(p.GetTruePeakDb())/20)
	return &gain, &peak
}

// coverURL is the largest album image, which Spotify serves from i.scdn.co.
// Modern metadata keeps the images in the cover group, ordered smallest first.
func coverURL(album *metadatapb.Album) string {
	images := album.GetCoverGroup().GetImage()
	if len(images) == 0 {
		images = album.GetCover()
	}
	return bestImageURL(images)
}
