package spotifyx

import (
	"container/list"
	"sync"
	"time"
)

// lru is a fixed-capacity least-recently-used map, safe for concurrent use.
// Metadata is immutable once built, so values are shared rather than copied.
type lru[K comparable, V any] struct {
	mu    sync.Mutex
	cap   int
	ll    *list.List
	byKey map[K]*list.Element
}

type lruEntry[K comparable, V any] struct {
	key K
	val V
}

func newLRU[K comparable, V any](capacity int) *lru[K, V] {
	if capacity < 1 {
		capacity = 1
	}
	return &lru[K, V]{
		cap:   capacity,
		ll:    list.New(),
		byKey: make(map[K]*list.Element, capacity),
	}
}

// get returns the value for k and marks it most recently used.
func (c *lru[K, V]) get(k K) (V, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if el, ok := c.byKey[k]; ok {
		c.ll.MoveToFront(el)
		return el.Value.(*lruEntry[K, V]).val, true
	}
	var zero V
	return zero, false
}

// put stores v for k, evicting the least recently used entry when full.
func (c *lru[K, V]) put(k K, v V) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if el, ok := c.byKey[k]; ok {
		el.Value.(*lruEntry[K, V]).val = v
		c.ll.MoveToFront(el)
		return
	}
	el := c.ll.PushFront(&lruEntry[K, V]{key: k, val: v})
	c.byKey[k] = el
	for c.ll.Len() > c.cap {
		oldest := c.ll.Back()
		if oldest == nil {
			break
		}
		c.ll.Remove(oldest)
		delete(c.byKey, oldest.Value.(*lruEntry[K, V]).key)
	}
}

func (c *lru[K, V]) length() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.ll.Len()
}

// ttEntry is a value with an expiry.
type ttEntry[V any] struct {
	val     V
	expires time.Time
}

// ttlLRU is an lru whose entries expire; expired entries are dropped on read.
// now is a field so tests can move the clock instead of sleeping.
type ttlLRU[K comparable, V any] struct {
	*lru[K, ttEntry[V]]
	ttl time.Duration
	now func() time.Time
}

func newTTLLRU[K comparable, V any](capacity int, ttl time.Duration) *ttlLRU[K, V] {
	return &ttlLRU[K, V]{
		lru: newLRU[K, ttEntry[V]](capacity),
		ttl: ttl,
		now: time.Now,
	}
}

func (c *ttlLRU[K, V]) get(k K) (V, bool) {
	e, ok := c.lru.get(k)
	if !ok || c.now().After(e.expires) {
		var zero V
		return zero, false
	}
	return e.val, true
}

func (c *ttlLRU[K, V]) put(k K, v V) {
	c.lru.put(k, ttEntry[V]{val: v, expires: c.now().Add(c.ttl)})
}
