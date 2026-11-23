package spotifyx

import (
	"context"
	"errors"
	"fmt"
	"io"
	"reflect"
	"time"

	librespot "github.com/devgianlu/go-librespot"
	"github.com/devgianlu/go-librespot/player"
	extmetadatapb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata"
	audiofilespb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata/audiofiles"
	metadatapb "github.com/devgianlu/go-librespot/proto/spotify/metadata"

	"foo_librespot/bridge/internal/seek"
	"foo_librespot/bridge/internal/spotifyuri"
)

const (
	// seekGuardMs is how far before the requested position the decoder is
	// opened, so that its coarse seek lands before the target and the anchor
	// scan can move forwards. Vorbis seeks can land a page (~140 ms) after the
	// requested position.
	seekGuardMs = 600
	// anchorLimitFrames bounds the anchor scan (two page boundaries).
	anchorLimitFrames = 3 * seek.SampleRate
	// zeroStartLimitMs is the start position under which a track is decoded
	// from the beginning instead of seeking, which is both exact and cheap.
	zeroStartLimitMs = 1000
)

// OpenResult is an opened track ready to be streamed.
type OpenResult struct {
	Info       TrackInfo
	SampleRate int32
	// StartFrame is the absolute sample frame of the first frame the Source
	// will return; it is exactly seek.FrameForMs(requested start).
	StartFrame int64
	DurationMs int32
	Source     librespot.AudioSource
	// EndOfTrack is set when the requested position is at/after the end: the
	// header is valid but there is no audio to send.
	EndOfTrack bool
	// FromCache is true when the encrypted audio file came from the on-disk
	// audio cache.
	FromCache bool
}

// Close releases the decoder behind an OpenResult.
func (r *OpenResult) Close() {
	if r.Source != nil {
		closeSource(r.Source)
		r.Source = nil
	}
}

// closeSource releases decoder resources. The decoders' Close methods have
// different signatures, hence the type switch rather than io.Closer.
func closeSource(src librespot.AudioSource) {
	switch s := src.(type) {
	case interface{ Close() }:
		s.Close()
	case interface{ Close() error }:
		_ = s.Close()
	}
}

// Open starts a decoded stream at startMs (sample-exact) for a track reference.
func (m *Manager) Open(ctx context.Context, ref spotifyuri.Ref, startMs int64, bitrate int) (*OpenResult, error) {
	if ref.Kind != spotifyuri.KindTrack {
		return nil, fmt.Errorf("%s is not a track", ref.URI)
	}
	if startMs < 0 {
		return nil, fmt.Errorf("start_ms must not be negative")
	}
	switch bitrate {
	case 0:
		bitrate = 320
	case 96, 160, 320:
	default:
		return nil, fmt.Errorf("bitrate must be 96, 160 or 320")
	}

	id, err := librespot.SpotifyIdFromUri(ref.URI)
	if err != nil {
		return nil, err
	}

	var result *OpenResult
	began := time.Now()
	hitsBefore := m.cacheHits.Load()
	err = m.Do(ctx, func(env *Env) error {
		var err error
		result, err = m.openOnce(ctx, env, ref, *id, startMs, bitrate)
		return err
	})
	if err != nil {
		m.log.Warnf("open %s failed after %s: %v", ref.URI, time.Since(began).Round(time.Millisecond), err)
		return nil, err
	}
	// Opens run concurrently, so this can over-attribute a hit to a neighbour;
	// it is exact for the usual one-open-at-a-time playback.
	result.FromCache = m.cacheHits.Load() > hitsBefore
	source := "network"
	if result.FromCache {
		source = "cache"
	}
	m.log.Infof("opened %s at %d ms from %s in %s", ref.URI, startMs, source, time.Since(began).Round(time.Millisecond))
	return result, nil
}

func (m *Manager) openOnce(ctx context.Context, env *Env, ref spotifyuri.Ref, id librespot.SpotifyId, startMs int64, bitrate int) (*OpenResult, error) {
	startFrame := seek.FrameForMs(startMs)

	st, anchor, err := m.openAt(ctx, env, id, bitrate, startMs, startFrame)
	if err != nil {
		return nil, err
	}

	// A position at or past the end: the library would start from the
	// beginning, but the client asked for a finished track.
	if duration := int64(st.Media.Duration()); duration > 0 && startMs >= duration {
		closeSource(st.Source)
		infos, err := m.infoWithoutStream(ctx, env, ref, bitrate)
		if err != nil {
			return nil, err
		}
		return &OpenResult{
			Info:       infos,
			SampleRate: st.SampleRate,
			StartFrame: startFrame,
			DurationMs: st.Media.Duration(),
			EndOfTrack: true,
		}, nil
	}

	if discard := seek.FramesToDiscard(startFrame, anchor); discard > 0 {
		if err := seek.Discard(st.Source, discard); err != nil {
			if errors.Is(err, io.EOF) {
				// The track is shorter than its metadata says; there is
				// nothing left to send.
				info := m.buildStreamInfo(ctx, env, ref, st)
				closeSource(st.Source)
				return &OpenResult{
					Info:       info,
					SampleRate: st.SampleRate,
					StartFrame: startFrame,
					DurationMs: st.Media.Duration(),
					EndOfTrack: true,
				}, nil
			}
			closeSource(st.Source)
			return nil, fmt.Errorf("discarding %d frames to reach %d ms: %w", discard, startMs, err)
		}
	}

	info := m.buildStreamInfo(ctx, env, ref, st)
	info.Playable = true

	return &OpenResult{
		Info:       info,
		SampleRate: st.SampleRate,
		StartFrame: startFrame,
		DurationMs: st.Media.Duration(),
		Source:     st.Source,
	}, nil
}

// openAt opens the track at a coarse position and anchors the decoder to an
// exact sample frame. It retries further back when a seek lands after the
// target, and falls back to decoding from the beginning when the decoder does
// not expose a usable position.
func (m *Manager) openAt(ctx context.Context, env *Env, id librespot.SpotifyId, bitrate int, startMs, startFrame int64) (*player.Stream, int64, error) {
	seeks := []int64{0}
	if startMs >= zeroStartLimitMs {
		seeks = []int64{startMs - seekGuardMs, startMs - 1500, startMs - 3000}
	}

	for _, seekMs := range seeks {
		if seekMs < 0 {
			seekMs = 0
		}
		st, err := m.NewStream(ctx, env, id, bitrate, seekMs)
		if err != nil {
			return nil, 0, err
		}

		anchor := int64(0)
		if seekMs > 0 {
			if isSampleExactSeek(st.File.GetFormat()) {
				// mp3 and flac seek by sample; trust the same arithmetic
				// librespot used, but verify for mp3 that the decoder really
				// landed there (no gapless info) before relying on it.
				anchor = seek.ExactSeekFrame(seekMs)
				if format := player.GetAudioFileFormatAudioFormat(st.File.GetFormat()); format == player.AudioFormatMP3 {
					if pos := st.Source.PositionMs(); pos < seekMs-2 || pos > seekMs+2 {
						closeSource(st.Source)
						m.log.Debugf("mp3 seek landed at %d ms, not %d ms; decoding from the start", pos, seekMs)
						break
					}
				}
			} else {
				anchor, err = seek.Anchor(st.Source, newGranuleReader(st.Source), anchorLimitFrames)
				if err != nil {
					closeSource(st.Source)
					m.log.WithError(err).Debugf("no exact anchor at %d ms, seeking further back", seekMs)
					continue
				}
				if anchor > startFrame {
					m.log.Debugf("decoder anchored at frame %d, after the target %d; seeking earlier", anchor, startFrame)
					closeSource(st.Source)
					continue
				}
			}
		}
		return st, anchor, nil
	}

	st, err := m.NewStream(ctx, env, id, bitrate, 0)
	if err != nil {
		return nil, 0, err
	}
	return st, 0, nil
}

// isSampleExactSeek reports whether the codec's SetPositionMs lands on the
// sample it was asked for, which makes the position known without anchoring.
func isSampleExactSeek(format metadatapb.AudioFile_Format) bool {
	switch player.GetAudioFileFormatAudioFormat(format) {
	case player.AudioFormatMP3, player.AudioFormatFLAC:
		return true
	default:
		return false
	}
}

// buildStreamInfo describes the stream that was actually opened, keeping the
// requested URI even when Spotify relinked the track to an alternative.
func (m *Manager) buildStreamInfo(ctx context.Context, env *Env, ref spotifyuri.Ref, st *player.Stream) TrackInfo {
	meta := &trackMeta{Audio: m.audioFiles(ctx, env, st.Media.Id())}
	if st.Media.IsTrack() {
		meta.Track = st.Media.Track()
	}

	info := BuildTrackInfo(ref.URI, meta, 320)
	if hg, ok := headerGains(st.Source); ok {
		// Debug aid for checking the AUDIO_FILES values against the ogg header's
		// own gains (the header's reference level differs from ReplayGain's).
		m.log.Debugf("ogg header gains for %s: track %.2f dB (peak %.4f), album %.2f dB (peak %.4f); AUDIO_FILES loudness-based track gain %s, album gain %s",
			ref.URI, hg[0], hg[1], hg[2], hg[3], fmtGain(info.TrackGainDB), fmtGain(info.AlbumGainDB))
	}
	info.Codec = player.GetFormatCodec(st.File.GetFormat())
	info.Bitrate = player.GetFormatBitrate(st.File.GetFormat())
	return info
}

func (m *Manager) infoWithoutStream(ctx context.Context, env *Env, ref spotifyuri.Ref, bitrate int) (TrackInfo, error) {
	metas, err := fetchTracks(ctx, env, []string{ref.URI})
	if err != nil {
		return TrackInfo{}, err
	}
	return BuildTrackInfo(ref.URI, metas[ref.URI], bitrate), nil
}

// audioFiles fetches the loudness metadata of the resolved track. It is
// advisory: a failure leaves the gains null instead of failing the open.
func (m *Manager) audioFiles(ctx context.Context, env *Env, id librespot.SpotifyId) *audiofilespb.AudioFilesExtensionResponse {
	tctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()

	var resp audiofilespb.AudioFilesExtensionResponse
	err := env.Session.Spclient().ExtendedMetadataSimple(tctx, id, extmetadatapb.ExtensionKind_AUDIO_FILES, &resp)
	if err != nil {
		m.log.WithError(err).Warnf("fetching loudness metadata for %s", id.Uri())
		return nil
	}
	return &resp
}

// granuleReader reads the exact sample position of the last decoded page from a
// decoder. librespot's AudioSource interface truncates PositionMs to
// milliseconds; the Vorbis decoder keeps the granule position, which is sample
// accurate, in an unexported field. Reflection keeps this optional: decoders
// without the field simply report no granule and the caller falls back.
type granuleReader struct {
	field reflect.Value
	ok    bool
}

func newGranuleReader(src librespot.AudioSource) seek.Granule {
	v := reflect.ValueOf(src)
	if v.Kind() == reflect.Pointer && !v.IsNil() {
		v = v.Elem()
	}
	g := &granuleReader{}
	if v.Kind() == reflect.Struct {
		if f := v.FieldByName("lastGranulepos"); f.IsValid() && (f.Kind() == reflect.Int64 || f.Kind() == reflect.Int) {
			g.field, g.ok = f, true
		}
	}
	return g.get
}

func (g *granuleReader) get() (int64, bool) {
	if !g.ok {
		return 0, false
	}
	return g.field.Int(), true
}

// headerGains reads the replay-gain segment of the Spotify ogg metadata page
// (track gain, track peak, album gain, album peak) from the Vorbis decoder's
// unexported state. It exists for a debug log line only: nothing depends on it,
// and any change in the library's layout simply yields ok=false.
func headerGains(src librespot.AudioSource) (gains [4]float64, ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	v := reflect.ValueOf(src)
	if v.Kind() == reflect.Pointer && !v.IsNil() {
		v = v.Elem()
	}
	if v.Kind() != reflect.Struct {
		return gains, false
	}
	meta := v.FieldByName("meta")
	if !meta.IsValid() || meta.Kind() != reflect.Pointer || meta.IsNil() {
		return gains, false
	}
	meta = meta.Elem()
	for i, name := range []string{"trackGainDb", "trackPeak", "albumGainDb", "albumPeak"} {
		f := meta.FieldByName(name)
		if !f.IsValid() || (f.Kind() != reflect.Float32 && f.Kind() != reflect.Float64) {
			return gains, false
		}
		gains[i] = f.Float()
	}
	return gains, true
}

func fmtGain(g *float64) string {
	if g == nil {
		return "none"
	}
	return fmt.Sprintf("%.2f dB", *g)
}
