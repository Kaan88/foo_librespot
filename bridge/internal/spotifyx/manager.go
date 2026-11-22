// Package spotifyx owns the shared Spotify session the bridge uses: lazy
// creation, recreation on failure, the account country, metadata lookups and
// decoded track streams.
package spotifyx

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	librespot "github.com/devgianlu/go-librespot"
	"github.com/devgianlu/go-librespot/ap"
	"github.com/devgianlu/go-librespot/player"
	devicespb "github.com/devgianlu/go-librespot/proto/spotify/connectstate/devices"
	"github.com/devgianlu/go-librespot/session"
	"github.com/devgianlu/go-librespot/spclient"

	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
)

// ErrNotLoggedIn is returned when no usable credentials file exists.
var ErrNotLoggedIn = errors.New("not logged in")

// Env is one session generation with its player.
type Env struct {
	Session *session.Session
	Player  *player.Player
}

// Manager owns the lazily created session shared by every pipe connection.
type Manager struct {
	log *logging.Logger

	// fixedCreds, when set, is used instead of the on-disk credentials file.
	fixedCreds *creds.Credentials

	ctx    context.Context
	cancel context.CancelFunc

	// opMu is held for reading while an operation uses the current session and
	// for writing while it is replaced. Streaming keeps working on the decoder
	// it was handed, so it does not hold the lock.
	opMu sync.RWMutex
	// createMu serialises session creation without holding opMu.
	createMu sync.Mutex

	mu     sync.Mutex
	env    *Env
	user   string
	lastCC atomic.Value // string

	// streamMu serialises player.NewStream: its CDN quarantine map is not
	// concurrency safe.
	streamMu sync.Mutex
}

// NewManager returns a manager bound to ctx.
func NewManager(ctx context.Context, log *logging.Logger) *Manager {
	ctx, cancel := context.WithCancel(ctx)
	return &Manager{log: log, ctx: ctx, cancel: cancel}
}

// NewManagerWithCredentials returns a manager that always uses c instead of
// reading the credentials file, which keeps the phase-0 spike on the same code
// path without changing where it stores its login.
func NewManagerWithCredentials(ctx context.Context, log *logging.Logger, c *creds.Credentials) *Manager {
	m := NewManager(ctx, log)
	m.fixedCreds = c
	return m
}

// Close releases the current session and stops accepting new work.
func (m *Manager) Close() {
	m.cancel()
	m.reset()
}

// Country is the account's country code, or "" until the accesspoint said.
func (m *Manager) Country() string {
	if v, ok := m.lastCC.Load().(string); ok {
		return v
	}
	return ""
}

// Username is the logged-in account name, known once a session exists.
func (m *Manager) Username() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.user
}

// WaitCountry waits up to timeout for the accesspoint to report the country.
func (m *Manager) WaitCountry(timeout time.Duration) string {
	deadline := time.Now().Add(timeout)
	for {
		if c := m.Country(); c != "" {
			return c
		}
		if time.Now().After(deadline) {
			return m.Country()
		}
		select {
		case <-m.ctx.Done():
			return m.Country()
		case <-time.After(50 * time.Millisecond):
		}
	}
}

// Do runs fn with the current session and, when fn fails with something that
// looks like a dead session, recreates it once and retries. A failure to create
// the session at all is returned as-is: session.NewSessionFromOptions already
// retries internally, and a second round would only delay the error.
func (m *Manager) Do(ctx context.Context, fn func(*Env) error) error {
	m.opMu.RLock()
	env, err := m.acquire()
	if err != nil {
		m.opMu.RUnlock()
		return err
	}
	err = fn(env)
	m.opMu.RUnlock()

	if err == nil || IsPermanent(err) || ctx.Err() != nil {
		return err
	}
	m.log.WithError(err).Warnf("request failed, recreating the Spotify session and retrying")
	m.reset()

	m.opMu.RLock()
	defer m.opMu.RUnlock()
	env, err = m.acquire()
	if err != nil {
		return err
	}
	return fn(env)
}

// IsPermanent reports whether err is a definitive answer that another session
// will not change (a refused or unplayable track), as opposed to a failure that
// may be caused by a dead session or connection.
func IsPermanent(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, librespot.ErrMediaRestricted) || errors.Is(err, librespot.ErrNoSupportedFormats) {
		return true
	}
	if errors.Is(err, ErrNotLoggedIn) {
		return true
	}
	var cre *spclient.ContextResolveError
	if errors.As(err, &cre) && cre.IsAccessDenied() {
		return true
	}
	return false
}

// acquire returns the current session generation, creating it if needed.
func (m *Manager) acquire() (*Env, error) {
	m.mu.Lock()
	if m.env != nil {
		env := m.env
		m.mu.Unlock()
		return env, nil
	}
	m.mu.Unlock()

	m.createMu.Lock()
	defer m.createMu.Unlock()

	m.mu.Lock()
	if m.env != nil {
		env := m.env
		m.mu.Unlock()
		return env, nil
	}
	m.mu.Unlock()

	env, user, err := m.create()
	if err != nil {
		return nil, err
	}

	m.mu.Lock()
	m.env = env
	m.user = user
	m.mu.Unlock()
	return env, nil
}

func (m *Manager) create() (*Env, string, error) {
	c := m.fixedCreds
	if c == nil {
		var err error
		c, err = creds.Load()
		if err != nil {
			if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
				return nil, "", err
			}
			return nil, "", fmt.Errorf("%w: %v", ErrNotLoggedIn, err)
		}
	}

	start := time.Now()
	m.log.Infof("creating Spotify session as %s", c.Username)

	ctx, cancel := context.WithTimeout(m.ctx, 60*time.Second)
	defer cancel()

	sess, err := session.NewSessionFromOptions(ctx, &session.Options{
		Log:         m.log,
		DeviceType:  devicespb.DeviceType(c.DeviceType),
		DeviceId:    c.DeviceID,
		Client:      librespot.NewHttpClient(),
		Credentials: session.StoredCredentials{Username: c.Username, Data: c.Data},
	})
	if err != nil {
		return nil, "", fmt.Errorf("opening Spotify session: %w", err)
	}

	p, err := player.NewPlayer(&player.Options{
		Spclient:             sess.Spclient(),
		AudioKey:             sess.AudioKey(),
		Events:               sess.Events(),
		Log:                  m.log,
		CountryCode:          m.Country,
		NormalisationEnabled: false,
	})
	if err != nil {
		sess.Close()
		return nil, "", fmt.Errorf("creating player: %w", err)
	}

	// The accesspoint reports the account country right after authentication;
	// the player needs it to apply media restrictions and alternative-track
	// relinking. Keep draining so a reconnect's packet cannot block the reader.
	go m.trackCountry(sess)

	m.log.Infof("Spotify session created in %s (username %s)", time.Since(start).Round(time.Millisecond), sess.Username())
	return &Env{Session: sess, Player: p}, sess.Username(), nil
}

func (m *Manager) trackCountry(sess *session.Session) {
	ch := sess.Accesspoint().Receive(ap.PacketTypeCountryCode)
	for pkt := range ch {
		if code := string(pkt.Payload); code != "" {
			m.lastCC.Store(code)
		}
	}
}

// reset closes the current session generation so the next request builds one.
func (m *Manager) reset() {
	m.opMu.Lock()
	defer m.opMu.Unlock()
	m.createMu.Lock()
	defer m.createMu.Unlock()

	m.mu.Lock()
	env := m.env
	m.env = nil
	m.user = ""
	m.mu.Unlock()

	if env == nil {
		return
	}
	env.Player.Close()
	env.Session.Close()
	m.log.Infof("Spotify session closed")
}

// NewStream opens a decoded stream, serialising the player call because the
// library's CDN bookkeeping is not concurrency safe.
func (m *Manager) NewStream(ctx context.Context, env *Env, id librespot.SpotifyId, bitrate int, posMs int64) (*player.Stream, error) {
	m.streamMu.Lock()
	defer m.streamMu.Unlock()
	return env.Player.NewStream(ctx, librespot.NewHttpClient(), id, bitrate, posMs)
}
