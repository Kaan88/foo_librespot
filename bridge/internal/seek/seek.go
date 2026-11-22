// Package seek turns a millisecond start position into an exact sample frame and
// moves a decoder there, discarding whatever it overshoots by.
//
// librespot seeks with millisecond granularity and lands on the decoder's own
// keyframe/page boundaries, which for Vorbis can be more than 100 ms before the
// requested position. The bridge reports the sample frame it actually starts at
// and drops decoded frames until the first one sent is exactly the target.
package seek

import (
	"errors"
	"math"
)

const (
	// SampleRate is the only rate librespot decodes to.
	SampleRate = 44100
	// Channels is the interleaved channel count of the decoded audio.
	Channels = 2
)

// ErrNoAnchor reports that a decoder's position could not be pinned down; the
// caller should start from the beginning of the track instead.
var ErrNoAnchor = errors.New("seek: could not determine the decoder position")

// FrameForMs is the frame index of a millisecond position: round(ms * 44100 /
// 1000). Rounding (not truncation) keeps repeated seeks idempotent.
func FrameForMs(ms int64) int64 {
	if ms <= 0 {
		return 0
	}
	return int64(math.Round(float64(ms) * SampleRate / 1000))
}

// ExactSeekFrame is where a decoder whose SetPositionMs is sample-accurate
// (mp3, flac) starts: SetPositionMs computes pos * rate / 1000 with integer
// arithmetic.
func ExactSeekFrame(ms int64) int64 {
	if ms <= 0 {
		return 0
	}
	return ms * SampleRate / 1000
}

// FramesToDiscard is how many frames to drop to move from decoderFrame to
// target. It is negative when the decoder landed after the target.
func FramesToDiscard(target, decoderFrame int64) int64 {
	return target - decoderFrame
}

// Source is the part of librespot.AudioSource the anchor code uses.
type Source interface {
	Read([]float32) (int, error)
	PositionMs() int64
}

// Granule reports the decoder's exact sample position at the end of the last
// decoded page, and whether the decoder exposes one at all. It is the only
// sample-accurate clock available: PositionMs truncates to milliseconds.
type Granule func() (int64, bool)

// Anchor watches the decoder cross two page boundaries and returns the absolute
// frame of the source's current read position.
//
// The granule read at open can be stale (it may still describe the page the
// seek scanned past), so the first transition is ignored. A transition happens
// when a read drains the buffered page and decodes the next one; the granule
// seen just before it is exactly the first sample of the new page, and the read
// that observed the change has already returned that sample, hence the +1.
func Anchor(src Source, granule Granule, maxFrames int64) (int64, error) {
	frame := make([]float32, Channels)

	prev, ok := granule()
	if !ok {
		return 0, ErrNoAnchor
	}

	var pos int64
	changes := 0
	for pos < maxFrames {
		if _, err := src.Read(frame); err != nil {
			return 0, ErrNoAnchor
		}
		pos++

		cur, ok := granule()
		if !ok {
			return 0, ErrNoAnchor
		}
		if cur == prev {
			continue
		}

		changes++
		if changes == 2 {
			return prev + 1, nil
		}
		prev = cur
	}

	return 0, ErrNoAnchor
}

// Discard drops n frames from the source, reading into a reusable buffer.
func Discard(src Source, n int64) error {
	if n <= 0 {
		return nil
	}
	buf := make([]float32, 16384)
	for n > 0 {
		want := n * Channels
		if want > int64(len(buf)) {
			want = int64(len(buf))
		}
		got, err := src.Read(buf[:want])
		n -= int64(got) / Channels
		if err != nil {
			return err
		}
		if got == 0 {
			return errors.New("seek: decoder produced no samples while discarding")
		}
	}
	return nil
}
