package wav

import (
	"encoding/binary"
	"math"
	"os"
	"testing"
)

func TestHeaderAndSamples(t *testing.T) {
	f, err := os.CreateTemp(t.TempDir(), "*.wav")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()

	w, err := NewWriter(f, 44100, 2)
	if err != nil {
		t.Fatalf("NewWriter: %v", err)
	}

	samples := []float32{0, 0.5, -0.5, 1.5, -1.5, float32(math.NaN())}
	if err := w.WriteSamples(samples); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	if got, want := w.DataBytes(), uint32(len(samples)*2); got != want {
		t.Fatalf("DataBytes = %d, want %d", got, want)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	raw, err := os.ReadFile(f.Name())
	if err != nil {
		t.Fatal(err)
	}
	if len(raw) != headerSize+len(samples)*2 {
		t.Fatalf("file length = %d, want %d", len(raw), headerSize+len(samples)*2)
	}

	if string(raw[0:4]) != "RIFF" {
		t.Errorf("RIFF id = %q", raw[0:4])
	}
	if got, want := binary.LittleEndian.Uint32(raw[4:8]), uint32(36+len(samples)*2); got != want {
		t.Errorf("RIFF size = %d, want %d", got, want)
	}
	if string(raw[8:12]) != "WAVE" || string(raw[12:16]) != "fmt " || string(raw[36:40]) != "data" {
		t.Errorf("chunk ids wrong: %q %q %q", raw[8:12], raw[12:16], raw[36:40])
	}
	if got := binary.LittleEndian.Uint16(raw[20:22]); got != 1 {
		t.Errorf("audio format = %d, want 1", got)
	}
	if got := binary.LittleEndian.Uint16(raw[22:24]); got != 2 {
		t.Errorf("channels = %d, want 2", got)
	}
	if got := binary.LittleEndian.Uint32(raw[24:28]); got != 44100 {
		t.Errorf("sample rate = %d, want 44100", got)
	}
	if got, want := binary.LittleEndian.Uint32(raw[28:32]), uint32(44100*2*2); got != want {
		t.Errorf("byte rate = %d, want %d", got, want)
	}
	if got := binary.LittleEndian.Uint16(raw[32:34]); got != 4 {
		t.Errorf("block align = %d, want 4", got)
	}
	if got := binary.LittleEndian.Uint16(raw[34:36]); got != 16 {
		t.Errorf("bits per sample = %d, want 16", got)
	}
	if got, want := binary.LittleEndian.Uint32(raw[40:44]), uint32(len(samples)*2); got != want {
		t.Errorf("data size = %d, want %d", got, want)
	}

	want := []int16{0, 16384, -16384, 32767, -32768, 0}
	for i, exp := range want {
		got := int16(binary.LittleEndian.Uint16(raw[headerSize+i*2:]))
		if got != exp {
			t.Errorf("sample %d = %d, want %d", i, got, exp)
		}
	}
}

func TestRejectsBadFormat(t *testing.T) {
	f, err := os.CreateTemp(t.TempDir(), "*.wav")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()

	if _, err := NewWriter(f, 0, 2); err == nil {
		t.Error("expected error for zero sample rate")
	}
	if _, err := NewWriter(f, 44100, 0); err == nil {
		t.Error("expected error for zero channels")
	}
}
