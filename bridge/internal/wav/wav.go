// Package wav writes 16-bit PCM WAV files from interleaved float32 samples.
package wav

import (
	"encoding/binary"
	"fmt"
	"io"
	"math"
)

const (
	headerSize = 44
	// BitsPerSample is fixed: the bridge converts float32 PCM to signed 16-bit.
	BitsPerSample = 16
)

// Writer streams 16-bit PCM into a WAV container. It needs a seekable sink so
// the RIFF and data sizes can be patched in when it is closed.
type Writer struct {
	w           io.WriteSeeker
	sampleRate  int32
	channels    int32
	dataBytes   uint32
	headerWrote bool
	closed      bool
}

// NewWriter writes a 44-byte WAV header with zeroed sizes (see Close) and
// returns a writer for the sample data.
func NewWriter(w io.WriteSeeker, sampleRate, channels int32) (*Writer, error) {
	if sampleRate <= 0 {
		return nil, fmt.Errorf("wav: invalid sample rate %d", sampleRate)
	}
	if channels <= 0 {
		return nil, fmt.Errorf("wav: invalid channel count %d", channels)
	}

	ww := &Writer{w: w, sampleRate: sampleRate, channels: channels}
	if err := ww.writeHeader(0); err != nil {
		return nil, err
	}
	ww.headerWrote = true
	return ww, nil
}

// WriteSamples converts interleaved float32 samples in [-1, 1] to signed
// 16-bit PCM and appends them. Values outside the range are clipped.
func (w *Writer) WriteSamples(samples []float32) error {
	if w.closed {
		return fmt.Errorf("wav: writer is closed")
	}
	if len(samples) == 0 {
		return nil
	}

	buf := make([]byte, len(samples)*2)
	for i, s := range samples {
		binary.LittleEndian.PutUint16(buf[i*2:], uint16(floatToInt16(s)))
	}
	if _, err := w.w.Write(buf); err != nil {
		return fmt.Errorf("wav: writing samples: %w", err)
	}
	w.dataBytes += uint32(len(buf))
	return nil
}

// Close patches the header with the final data size. It does not close the
// underlying sink.
func (w *Writer) Close() error {
	if w.closed {
		return nil
	}
	w.closed = true

	if _, err := w.w.Seek(0, io.SeekStart); err != nil {
		return fmt.Errorf("wav: seeking to header: %w", err)
	}
	if err := w.writeHeader(w.dataBytes); err != nil {
		return err
	}
	if _, err := w.w.Seek(0, io.SeekEnd); err != nil {
		return fmt.Errorf("wav: seeking to end: %w", err)
	}
	return nil
}

// DataBytes reports how many bytes of PCM have been written so far.
func (w *Writer) DataBytes() uint32 {
	return w.dataBytes
}

func (w *Writer) writeHeader(dataBytes uint32) error {
	h := make([]byte, headerSize)

	block := uint16(int32(BitsPerSample/8) * w.channels)
	byteRate := uint32(w.sampleRate) * uint32(block)

	copy(h[0:4], "RIFF")
	binary.LittleEndian.PutUint32(h[4:8], 36+dataBytes)
	copy(h[8:12], "WAVE")
	copy(h[12:16], "fmt ")
	binary.LittleEndian.PutUint32(h[16:20], 16) // fmt chunk size
	binary.LittleEndian.PutUint16(h[20:22], 1)  // PCM
	binary.LittleEndian.PutUint16(h[22:24], uint16(w.channels))
	binary.LittleEndian.PutUint32(h[24:28], uint32(w.sampleRate))
	binary.LittleEndian.PutUint32(h[28:32], byteRate)
	binary.LittleEndian.PutUint16(h[32:34], block)
	binary.LittleEndian.PutUint16(h[34:36], BitsPerSample)
	copy(h[36:40], "data")
	binary.LittleEndian.PutUint32(h[40:44], dataBytes)

	if _, err := w.w.Write(h); err != nil {
		return fmt.Errorf("wav: writing header: %w", err)
	}
	return nil
}

func floatToInt16(f float32) int16 {
	if math.IsNaN(float64(f)) {
		return 0
	}
	v := math.Round(float64(f) * 32767.0)
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return int16(v)
}
