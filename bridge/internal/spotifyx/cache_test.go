package spotifyx

import (
	"testing"
	"time"
)

func TestLRUEvictsLeastRecentlyUsed(t *testing.T) {
	c := newLRU[string, int](2)
	c.put("a", 1)
	c.put("b", 2)
	if v, ok := c.get("a"); !ok || v != 1 {
		t.Fatalf("a = %d, %v", v, ok)
	}

	// b is now the least recently used, so c evicts it.
	c.put("c", 3)
	if _, ok := c.get("b"); ok {
		t.Fatal("b was not evicted")
	}
	if v, ok := c.get("a"); !ok || v != 1 {
		t.Errorf("a = %d, %v", v, ok)
	}
	if v, ok := c.get("c"); !ok || v != 3 {
		t.Errorf("c = %d, %v", v, ok)
	}
	if got := c.length(); got != 2 {
		t.Errorf("length = %d, want 2", got)
	}
}

func TestLRUUpdatesExistingKey(t *testing.T) {
	c := newLRU[string, int](2)
	c.put("a", 1)
	c.put("a", 2)
	if v, ok := c.get("a"); !ok || v != 2 {
		t.Fatalf("a = %d, %v", v, ok)
	}
	if got := c.length(); got != 1 {
		t.Errorf("length = %d, want 1", got)
	}
}

func TestTTLLRUExpires(t *testing.T) {
	c := newTTLLRU[string, int](4, 10*time.Second)
	now := time.Unix(1000, 0)
	c.now = func() time.Time { return now }

	c.put("a", 1)
	if v, ok := c.get("a"); !ok || v != 1 {
		t.Fatalf("fresh entry = %d, %v", v, ok)
	}

	now = now.Add(11 * time.Second)
	if _, ok := c.get("a"); ok {
		t.Fatal("expired entry was returned")
	}
}
