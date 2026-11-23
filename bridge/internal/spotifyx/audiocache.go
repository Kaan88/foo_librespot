package spotifyx

import (
	"encoding/xml"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"time"

	librespot "github.com/devgianlu/go-librespot"
	"github.com/devgianlu/go-librespot/ap"
	"github.com/devgianlu/go-librespot/cache"
	"github.com/devgianlu/go-librespot/player"
	"github.com/devgianlu/go-librespot/session"
)

// MiB is one mebibyte.
const MiB = 1 << 20

// CacheInfo describes the audio cache for the config reply.
type CacheInfo struct {
	// MB is the configured size limit in MiB; 0 means the cache is off.
	MB int
	// Dir is the cache root; the cached files live in its "audio" folder.
	Dir string
	// Bytes is the size of the cached audio files currently on disk.
	Bytes int64
}

// SetCacheDir sets where the cache lives. It must be called before
// ConfigureCache; without it the cache stays off.
func (m *Manager) SetCacheDir(dir string) {
	m.mu.Lock()
	m.cacheDir = dir
	m.mu.Unlock()
}

// CacheSettings returns the current cache settings and its size on disk.
func (m *Manager) CacheSettings() CacheInfo {
	m.mu.Lock()
	dir, mb := m.cacheDir, m.cacheMB
	m.mu.Unlock()
	return CacheInfo{MB: mb, Dir: dir, Bytes: dirSize(filepath.Join(dir, "audio"))}
}

// ConfigureCache turns the persistent audio cache on with a limit of mb MiB, or
// off for mb <= 0. A running session gets a new player with the new cache
// right away (streams already open keep decoding from what they hold); nothing
// reconnects.
func (m *Manager) ConfigureCache(mb int) error {
	if mb < 0 {
		mb = 0
	}
	m.mu.Lock()
	dir := m.cacheDir
	m.mu.Unlock()

	var c *cache.Cache
	if mb > 0 {
		if dir == "" {
			return fmt.Errorf("no cache directory configured")
		}
		var err error
		c, err = cache.New(m.log, dir, int64(mb)*MiB)
		if err != nil {
			return err
		}
	}

	m.mu.Lock()
	m.cacheMB = mb
	m.audioCache = c
	m.mu.Unlock()
	m.log.Infof("audio cache: %s", describeCache(dir, mb))
	m.swapPlayer()
	return nil
}

// ClearCache deletes every cached file and starts the cache over (when it is
// enabled). Files held open by a stream that is playing right now cannot be
// deleted on Windows; those are reported and left to the next clear.
func (m *Manager) ClearCache() error {
	m.mu.Lock()
	dir, mb := m.cacheDir, m.cacheMB
	m.mu.Unlock()
	if dir == "" {
		return nil
	}

	var firstErr error
	audioDir := filepath.Join(dir, "audio")
	entries, err := os.ReadDir(audioDir)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	// Streams that were closed still hold their cached file open until the
	// garbage collector finalises it, and Windows cannot delete an open file:
	// collect first, and retry what is still busy.
	runtime.GC()
	for _, e := range entries {
		path := filepath.Join(audioDir, e.Name())
		rerr := os.RemoveAll(path)
		for retry := 0; rerr != nil && retry < 3; retry++ {
			runtime.GC()
			time.Sleep(100 * time.Millisecond)
			rerr = os.RemoveAll(path)
		}
		if rerr != nil && firstErr == nil {
			firstErr = rerr
		}
	}
	m.log.Infof("audio cache cleared (%d entries)", len(entries))

	// Rebuild so the size accounting of the limiter starts from what is left.
	if mb > 0 {
		if cerr := m.ConfigureCache(mb); cerr != nil && firstErr == nil {
			firstErr = cerr
		}
	}
	if firstErr != nil {
		return fmt.Errorf("some cached files could not be deleted (in use?): %w", firstErr)
	}
	return nil
}

func describeCache(dir string, mb int) string {
	if mb <= 0 {
		return "off"
	}
	return fmt.Sprintf("on, limit %d MiB, dir %s", mb, dir)
}

// currentCache returns the active cache (nil when off).
func (m *Manager) currentCache() *cache.Cache {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.audioCache
}

// newPlayer builds the stream factory for sess with the active cache.
func (m *Manager) newPlayer(sess *session.Session) (*player.Player, error) {
	return player.NewPlayer(&player.Options{
		Spclient:             sess.Spclient(),
		AudioKey:             sess.AudioKey(),
		Events:               sess.Events(),
		Log:                  &hitLogger{Logger: m.log, hits: &m.cacheHits},
		Cache:                m.currentCache(),
		CountryCode:          m.Country,
		NormalisationEnabled: false,
	})
}

// swapPlayer replaces the current generation's player so it uses the active
// cache. Env.Player is only touched under the write lock that every user of
// the session holds for reading.
func (m *Manager) swapPlayer() {
	m.opMu.Lock()
	m.mu.Lock()
	env := m.env
	m.mu.Unlock()
	if env == nil || env.Session == nil {
		m.opMu.Unlock()
		return
	}
	p, err := m.newPlayer(env.Session)
	if err != nil {
		m.opMu.Unlock()
		m.log.WithError(err).Warnf("could not apply the cache setting to the running session; it applies after the next reconnect")
		return
	}
	old := env.Player
	env.Player = p
	m.opMu.Unlock()
	if old != nil {
		old.Close()
	}
}

// hitLogger passes everything to the bridge log and counts the library's
// "using cached audio file" line, which is the only signal that a stream came
// from the cache. NewStream calls are serialised (streamMu), so the counter
// delta around one call belongs to it.
type hitLogger struct {
	librespot.Logger
	hits *atomic.Int64
}

func (l *hitLogger) Debugf(format string, args ...interface{}) {
	if strings.HasPrefix(format, "using cached audio file") {
		l.hits.Add(1)
	}
	l.Logger.Debugf(format, args...)
}

func (l *hitLogger) WithField(key string, value interface{}) librespot.Logger {
	return &hitLogger{Logger: l.Logger.WithField(key, value), hits: l.hits}
}

func (l *hitLogger) WithError(err error) librespot.Logger {
	return &hitLogger{Logger: l.Logger.WithError(err), hits: l.hits}
}

// dirSize is the total size of regular files below dir (0 if it is missing).
func dirSize(dir string) int64 {
	var total int64
	_ = filepath.WalkDir(dir, func(_ string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		if info, ierr := d.Info(); ierr == nil && !strings.HasSuffix(d.Name(), ".tmp") {
			total += info.Size()
		}
		return nil
	})
	return total
}

// trackProduct keeps the account's product type up to date from the
// accesspoint's product-info packets (XML, like go-librespot's daemon reads).
func (m *Manager) trackProduct(sess *session.Session) {
	ch := sess.Accesspoint().Receive(ap.PacketTypeProductInfo)
	for pkt := range ch {
		if product := parseProduct(pkt.Payload); product != "" {
			m.product.Store(product)
			m.log.Infof("account product: %s", product)
		}
	}
}

// parseProduct returns the first product type of a product-info payload.
func parseProduct(payload []byte) string {
	var info struct {
		Products []struct {
			Type string `xml:"type"`
		} `xml:"product"`
	}
	if err := xml.Unmarshal(payload, &info); err != nil || len(info.Products) == 0 {
		return ""
	}
	return strings.ToLower(strings.TrimSpace(info.Products[0].Type))
}
