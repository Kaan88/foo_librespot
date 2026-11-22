package seek

import (
	"errors"
	"testing"
)

func TestFrameForMs(t *testing.T) {
	cases := []struct {
		ms   int64
		want int64
	}{
		{0, 0},
		{1, 44},          // 44.1 rounds to 44
		{2, 88},          // 88.2 -> 88
		{10, 441},        // 441
		{1000, 44100},    // 44100
		{60000, 2646000}, // 2646000
		{60001, 2646044}, // 2646044.1 -> 2646044
		{5, 221},         // 220.5 rounds to 221 (away from zero)
		{-5, 0},
	}
	for _, tc := range cases {
		if got := FrameForMs(tc.ms); got != tc.want {
			t.Errorf("FrameForMs(%d) = %d, want %d", tc.ms, got, tc.want)
		}
	}
}

func TestExactSeekFrame(t *testing.T) {
	if got := ExactSeekFrame(60000); got != 2646000 {
		t.Errorf("ExactSeekFrame(60000) = %d", got)
	}
	// Integer division, matching librespot's SetPositionMs.
	if got := ExactSeekFrame(1); got != 44 {
		t.Errorf("ExactSeekFrame(1) = %d, want 44", got)
	}
}

func TestFramesToDiscard(t *testing.T) {
	if got := FramesToDiscard(2646000, 2636032); got != 9968 {
		t.Errorf("FramesToDiscard = %d, want 9968", got)
	}
	if got := FramesToDiscard(100, 200); got != -100 {
		t.Errorf("FramesToDiscard(late) = %d, want -100", got)
	}
}

// fakeSource emits pages of samples and reports a granule that only advances
// when a new page is decoded, exactly like the Vorbis decoder. The page whose
// granule is reported at open is the one the coarse seek scanned past, so it is
// stale by design.
type fakeSource struct {
	pages   [][2]int64 // {startFrame, length} per page
	pos     int64      // frames returned so far
	page    int        // page currently buffered
	inPage  int64      // frames returned from the current page
	granule int64      // granule of the last decoded page
	openErr error
}

func newFake(pages [][2]int64, staleGranule int64) *fakeSource {
	return &fakeSource{pages: pages, granule: staleGranule}
}

func (f *fakeSource) Read(p []float32) (int, error) {
	if f.openErr != nil {
		return 0, f.openErr
	}
	if f.page >= len(f.pages) {
		return 0, errors.New("eof")
	}
	page := f.pages[f.page]
	if f.inPage >= page[1] {
		f.page++
		f.inPage = 0
		if f.page >= len(f.pages) {
			return 0, errors.New("eof")
		}
		page = f.pages[f.page]
		f.granule = page[0] + page[1]
	}
	n := int64(len(p) / 2)
	if rest := page[1] - f.inPage; n > rest {
		n = rest
	}
	for i := int64(0); i < n*2; i++ {
		p[i] = 0
	}
	f.inPage += n
	f.pos += n
	return int(n * 2), nil
}

func (f *fakeSource) PositionMs() int64 { return f.granule * 1000 / SampleRate }

func (f *fakeSource) granuleFn() (int64, bool) { return f.granule, true }

func TestAnchorIgnoresStaleGranule(t *testing.T) {
	// Pages of 4160 frames starting at 2636032. The decoder reports the
	// granule of a page it scanned past at open.
	pages := [][2]int64{
		{2636032, 4160},
		{2640192, 6144},
		{2646336, 4608},
		{2650944, 5120},
	}
	src := newFake(pages, 2635901)

	got, err := Anchor(src, src.granuleFn, 44100)
	if err != nil {
		t.Fatalf("Anchor: %v", err)
	}
	// The stale value hides the first transition, so the anchor lands on the
	// second one: page 3 starts at 2646336 and the read that observed the
	// change already returned its first frame.
	if got != 2646337 {
		t.Fatalf("Anchor = %d, want 2646337", got)
	}
}

func TestAnchorWithRealInitialGranule(t *testing.T) {
	// Opened at zero: the first granule is the real end of page 1.
	pages := [][2]int64{
		{0, 4096},
		{4096, 4096},
		{8192, 4096},
	}
	src := newFake(pages, 4096)

	got, err := Anchor(src, src.granuleFn, 44100)
	if err != nil {
		t.Fatalf("Anchor: %v", err)
	}
	if got != 8193 {
		t.Fatalf("Anchor = %d, want 8193", got)
	}
}

func TestAnchorWithoutGranule(t *testing.T) {
	src := newFake([][2]int64{{0, 100}}, 0)
	if _, err := Anchor(src, func() (int64, bool) { return 0, false }, 44100); !errors.Is(err, ErrNoAnchor) {
		t.Fatalf("Anchor error = %v, want ErrNoAnchor", err)
	}
}

func TestDiscard(t *testing.T) {
	src := newFake([][2]int64{{0, 5000}, {5000, 5000}}, 0)
	if err := Discard(src, 7000); err != nil {
		t.Fatalf("Discard: %v", err)
	}
	if src.pos != 7000 {
		t.Fatalf("discarded to %d frames, want 7000", src.pos)
	}
	if err := Discard(src, 0); err != nil {
		t.Fatalf("Discard(0): %v", err)
	}
}
