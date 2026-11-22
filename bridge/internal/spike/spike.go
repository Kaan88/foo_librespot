package spike

import (
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"strings"

	librespot "github.com/devgianlu/go-librespot"
	"github.com/devgianlu/go-librespot/player"
	devicespb "github.com/devgianlu/go-librespot/proto/spotify/connectstate/devices"
	extmetadatapb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata"
	audiofilespb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata/audiofiles"
	"github.com/devgianlu/go-librespot/session"

	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/wav"
)

// Login runs the OAuth 2.0 device authorization flow, prints the URL and code
// the user must approve, then persists the resulting stored credentials.
func Login(ctx context.Context, log *logging.Logger) (*creds.Credentials, error) {
	deviceID, err := creds.NewDeviceID()
	if err != nil {
		return nil, err
	}

	client := librespot.NewHttpClient()
	sess, err := session.NewSessionFromOptions(ctx, &session.Options{
		Log:        log,
		DeviceType: devicespb.DeviceType_COMPUTER,
		DeviceId:   deviceID,
		Client:     client,
		Credentials: session.DeviceAuthCredentials{
			OnCode: func(code *session.DeviceAuthCode) {
				if code == nil {
					return
				}
				fmt.Printf("\nOpen this URL in a browser: %s\n", code.VerificationUrl)
				fmt.Printf("If prompted, enter code: %s\n", code.UserCode)
				if !code.ExpiresAt.IsZero() {
					fmt.Printf("Code expires at: %s\n", code.ExpiresAt.Local().Format("15:04:05"))
				}
				fmt.Println("Waiting for approval...")
			},
		},
	})
	if err != nil {
		return nil, fmt.Errorf("login: %w", err)
	}

	c := &creds.Credentials{
		DeviceID:   deviceID,
		DeviceType: int32(devicespb.DeviceType_COMPUTER),
		Username:   sess.Username(),
		Data:       sess.StoredCredentials(),
	}
	sess.Close()

	if len(c.Data) == 0 {
		return nil, errors.New("login succeeded but the library returned no reusable credentials")
	}
	if err := c.Save(); err != nil {
		return nil, err
	}

	path, _ := creds.Path()
	fmt.Printf("Logged in as %s; credentials saved to %s\n", c.Username, path)
	return c, nil
}

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
	id, err := ParseTrackID(opts.Track)
	if err != nil {
		return nil, err
	}

	client := librespot.NewHttpClient()
	sess, err := session.NewSessionFromOptions(ctx, &session.Options{
		Log:         log,
		DeviceType:  devicespb.DeviceType(c.DeviceType),
		DeviceId:    c.DeviceID,
		Client:      client,
		Credentials: session.StoredCredentials{Username: c.Username, Data: c.Data},
	})
	if err != nil {
		return nil, fmt.Errorf("opening session: %w", err)
	}
	defer sess.Close()

	// The player is only used as the stream factory. No output backend is ever
	// opened because SetPrimaryStream is never called.
	p, err := player.NewPlayer(&player.Options{
		Spclient: sess.Spclient(),
		AudioKey: sess.AudioKey(),
		Events:   sess.Events(),
		Log:      log,
		// go-librespot calls CountryCode while resolving the track; an empty
		// value disables its restriction check, and this spike does not
		// implement Connect state reporting.
		CountryCode:          func() string { return "" },
		NormalisationEnabled: false,
	})
	if err != nil {
		return nil, fmt.Errorf("creating player: %w", err)
	}
	defer p.Close()

	fmt.Printf("Resolving %s (bitrate preference %d kbps)...\n", id.Uri(), opts.Bitrate)
	stream, err := p.NewStream(ctx, client, id, opts.Bitrate, opts.StartMs)
	if err != nil {
		return nil, fmt.Errorf("opening stream: %w", err)
	}

	res := &DumpResult{SampleRate: stream.SampleRate, Channels: player.Channels}
	res.PositionAtOpenMs = stream.Source.PositionMs()
	defer closeSource(stream.Source)

	format := stream.File.GetFormat()
	fmt.Printf("Track:    %s\n", stream.Media.Name())
	if stream.Media.IsTrack() {
		var artists []string
		for _, a := range stream.Media.Track().GetArtist() {
			artists = append(artists, a.GetName())
		}
		if len(artists) > 0 {
			fmt.Printf("Artist:   %s\n", strings.Join(artists, ", "))
		}
	}
	fmt.Printf("Duration: %d ms\n", stream.Media.Duration())
	fmt.Printf("Format:   %s (codec %s, nominal %d kbps, file id %s)\n",
		format.String(), player.GetFormatCodec(format), player.GetFormatBitrate(format),
		hex.EncodeToString(stream.File.GetFileId()))
	fmt.Printf("Decoded:  %d Hz, %d channels, bit depth %d (0 = lossy)\n",
		stream.SampleRate, player.Channels, stream.BitDepth)
	if opts.StartMs > 0 {
		fmt.Printf("Seek:     requested %d ms; decoder reports %d ms\n", opts.StartMs, res.PositionAtOpenMs)
		if opts.StartMs >= int64(stream.Media.Duration()) {
			fmt.Printf("Seek:     requested position is at/after the end; the library starts from 0\n")
		}
	}

	reportNormalisation(ctx, sess, stream)

	out, err := os.Create(opts.OutPath)
	if err != nil {
		return nil, fmt.Errorf("creating output: %w", err)
	}
	defer out.Close()

	ww, err := wav.NewWriter(out, stream.SampleRate, player.Channels)
	if err != nil {
		return nil, err
	}

	maxFrames := int64(-1)
	if opts.Seconds > 0 {
		maxFrames = int64(math.Round(opts.Seconds * float64(stream.SampleRate)))
	}

	const readFloats = 16384
	buf := make([]float32, readFloats)
	channels := int64(player.Channels)
	var frames int64
	var peak float32
	var sumSquares float64
	zeroReads := 0

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
			zeroReads = 0
		} else if readErr == nil {
			// Defensive: a decoder that returns (0, nil) should not spin.
			zeroReads++
			if zeroReads > 100 {
				return nil, errors.New("decoder returned no samples repeatedly")
			}
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
	res.DecodedMs = frames * 1000 / int64(stream.SampleRate)
	res.PositionAtEndMs = stream.Source.PositionMs()

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
		opts.OutPath, res.OutputBytes, frames, float64(frames)/float64(stream.SampleRate))
	if frames > 0 {
		rms := math.Sqrt(sumSquares / float64(frames*channels))
		fmt.Printf("Levels:   peak %.4f, rms %.4f (linear float)\n", peak, rms)
	}
	fmt.Printf("Position: decoder reports %d ms at end (expected about %d ms)\n",
		res.PositionAtEndMs, opts.StartMs+res.DecodedMs)
	if opts.StartMs > 0 && opts.StartMs < int64(stream.Media.Duration()) {
		if diff := res.PositionAtOpenMs - opts.StartMs; diff > 3000 || diff < -3000 {
			fmt.Printf("WARNING:  decoder opened at %d ms, %d ms away from the requested start\n", res.PositionAtOpenMs, diff)
		}
	}
	if frames == 0 {
		return nil, errors.New("stream produced no audio")
	}

	return res, nil
}

// reportNormalisation prints Spotify's loudness metadata for the track. The
// spike deliberately decodes without applying it, so the factor is advisory.
func reportNormalisation(ctx context.Context, sess *session.Session, stream *player.Stream) {
	var resp audiofilespb.AudioFilesExtensionResponse
	err := sess.Spclient().ExtendedMetadataSimple(ctx, stream.RequestedId, extmetadatapb.ExtensionKind_AUDIO_FILES, &resp)
	if err != nil {
		fmt.Printf("Loudness: unavailable (%v)\n", err)
		return
	}

	params := resp.GetDefaultFileNormalizationParams()
	if params == nil {
		fmt.Println("Loudness: no normalisation parameters for this track")
		return
	}

	// Same maths as go-librespot's calculateNormalisationFactor with no pre-gain.
	gainDb := -14.0 - params.GetLoudnessDb()
	factor := math.Pow(10, float64(gainDb)/20)
	truePeakLinear := math.Pow(10, float64(params.GetTruePeakDb())/20)
	if truePeakLinear > 0 && factor*truePeakLinear > 1 {
		factor = 1 / truePeakLinear
	}

	fmt.Printf("Loudness: %.2f LUFS, true peak %.2f dBTP; unclamped gain %+.2f dB, factor %.4f (not applied)\n",
		params.GetLoudnessDb(), params.GetTruePeakDb(), gainDb, factor)
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
