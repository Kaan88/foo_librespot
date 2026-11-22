package spike

import (
	"context"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"strings"

	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/spotifyuri"
	"foo_librespot/bridge/internal/spotifyx"
	"foo_librespot/bridge/internal/wav"
)

// DumpOptions describes one track extraction.
type DumpOptions struct {
	Track   string
	OutPath string
	StartMs int64
	Seconds float64
	Bitrate int
}

// DumpResult is the measurable outcome of a dump.
type DumpResult struct {
	Frames           int64
	SampleRate       int32
	Channels         int32
	DecodedMs        int64
	PositionAtOpenMs int64
	PositionAtEndMs  int64
	OutputBytes      int64
}

// Dump fetches and decodes one track, writing 16-bit PCM WAV. It never opens an
// audio output device: it reads the decoded float32 source directly.
func Dump(ctx context.Context, log *logging.Logger, c *creds.Credentials, opts DumpOptions) (*DumpResult, error) {
	ref, err := spotifyuri.Parse(opts.Track)
	if err != nil {
		return nil, err
	}
	if ref.Kind != spotifyuri.KindTrack {
		return nil, fmt.Errorf("%s is not a track", ref.URI)
	}

	mgr := spotifyx.NewManagerWithCredentials(ctx, log, c)
	defer mgr.Close()

	fmt.Printf("Resolving %s (bitrate preference %d kbps)...\n", ref.URI, opts.Bitrate)
	stream, err := mgr.Open(ctx, ref, opts.StartMs, opts.Bitrate)
	if err != nil {
		return nil, fmt.Errorf("opening stream: %w", err)
	}
	defer stream.Close()

	res := &DumpResult{
		SampleRate: stream.SampleRate,
		Channels:   int32(2),
	}
	res.PositionAtOpenMs = stream.StartFrame * 1000 / 44100

	fmt.Printf("Track:    %s\n", stream.Info.Title)
	if len(stream.Info.Artists) > 0 {
		fmt.Printf("Artist:   %s\n", strings.Join(stream.Info.Artists, ", "))
	}
	fmt.Printf("Album:    %s (%s)\n", stream.Info.Album, stream.Info.Date)
	fmt.Printf("Duration: %d ms\n", stream.DurationMs)
	fmt.Printf("Format:   %s, %d kbps\n", stream.Info.Codec, stream.Info.Bitrate)
	if stream.Info.TrackGainDB != nil {
		fmt.Printf("Loudness: gain %+.2f dB, peak %.4f (not applied)\n",
			*stream.Info.TrackGainDB, *stream.Info.TrackPeak)
	}
	fmt.Printf("Decoded:  %d Hz, %d channels\n", res.SampleRate, res.Channels)
	if opts.StartMs > 0 {
		fmt.Printf("Seek:     requested %d ms; first frame %d (%.3f ms)\n",
			opts.StartMs, stream.StartFrame, float64(stream.StartFrame)/44.1)
	}

	out, err := os.Create(opts.OutPath)
	if err != nil {
		return nil, fmt.Errorf("creating output: %w", err)
	}
	defer out.Close()

	ww, err := wav.NewWriter(out, res.SampleRate, res.Channels)
	if err != nil {
		return nil, err
	}

	maxFrames := int64(-1)
	if opts.Seconds > 0 {
		maxFrames = int64(math.Round(opts.Seconds * float64(res.SampleRate)))
	}

	const readFloats = 16384
	buf := make([]float32, readFloats)
	channels := int64(res.Channels)
	var frames int64
	var peak float32
	var sumSquares float64

readLoop:
	for {
		n, readErr := stream.Source.Read(buf)
		if n > 0 {
			samples := buf[:n]
			if maxFrames >= 0 {
				remaining := (maxFrames - frames) * channels
				if remaining <= 0 {
					break
				}
				if int64(len(samples)) > remaining {
					samples = samples[:remaining]
				}
			}

			for _, s := range samples {
				abs := float32(math.Abs(float64(s)))
				if abs > peak {
					peak = abs
				}
				sumSquares += float64(s) * float64(s)
			}

			if err := ww.WriteSamples(samples); err != nil {
				return nil, err
			}
			frames += int64(len(samples)) / channels
		}

		if readErr != nil {
			if errors.Is(readErr, io.EOF) {
				break
			}
			return nil, fmt.Errorf("reading decoded audio: %w", readErr)
		}
		if maxFrames >= 0 && frames >= maxFrames {
			break readLoop
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		default:
		}
	}

	res.Frames = frames
	res.DecodedMs = frames * 1000 / int64(res.SampleRate)
	res.PositionAtEndMs = res.PositionAtOpenMs + res.DecodedMs

	if err := ww.Close(); err != nil {
		return nil, err
	}
	if err := out.Close(); err != nil {
		return nil, fmt.Errorf("closing output: %w", err)
	}
	if info, err := os.Stat(opts.OutPath); err == nil {
		res.OutputBytes = info.Size()
	}

	fmt.Printf("Wrote:    %s (%d bytes, %d frames, %.2f s)\n",
		opts.OutPath, res.OutputBytes, frames, float64(frames)/float64(res.SampleRate))
	if frames > 0 {
		rms := math.Sqrt(sumSquares / float64(frames*channels))
		fmt.Printf("Levels:   peak %.4f, rms %.4f (linear float)\n", peak, rms)
	}
	if frames == 0 {
		return nil, errors.New("stream produced no audio")
	}

	return res, nil
}
