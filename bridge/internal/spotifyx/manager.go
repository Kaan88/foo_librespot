// Package spotifyx owns the shared Spotify session the bridge uses: eager
// background creation with single-flight retries, the account country,
// metadata lookups and decoded track streams.
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
	"github.com/devgianlu/go-librespot/apresolve"
	"github.com/devgianlu/go-librespot/audio"
	"github.com/devgianlu/go-librespot/cache"
	"github.com/devgianlu/go-librespot/player"
	devicespb "github.com/devgianlu/go-librespot/proto/spotify/connectstate/devices"
	"github.com/devgianlu/go-librespot/session"
	"github.com/devgianlu/go-librespot/spclient"

	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
)

// ErrNotLoggedIn is returned when no usable credentials file exists.
var ErrNotLoggedIn = errors.New("not logged in")

// Session states reported by status.
const (
	StateConnecting = "connecting"
	StateConnected  = "connected"
	StateError      = "error"
)

// Connection tuning. A single accesspoint that stops answering must not hold
// the whole bridge for the library's 30s handshake timeout: each attempt gets
// its own short deadline and the next accesspoint is tried right after.
const (
	// sessionAttemptTimeout bounds one accesspoint attempt.
	sessionAttemptTimeout = 10 * time.Second
	// sessionAttempts is how many accesspoints one creation round tries.
	sessionAttempts = 3
	// retryBase and retryMax bound the pause between failed creation rounds.
	retryBase = 2 * time.Second
	retryMax  = 30 * time.Second
)

// Env is one session generation with its player.
type Env struct {
	Session *session.Session
	Player  *player.Player
}

// Status is the snapshot the status request answers with.
type Status struct {
	// LoggedIn reports whether credentials exist, not whether Spotify accepted
	// them: it is known immediately and is what decides "run login".
	LoggedIn bool
	// State is StateConnecting, StateConnected or StateError.
	State string
	// LastError is the reason of the last failed creation, if any.
	LastError string
	// Username comes from the credentials, so it is known while connecting.
	Username string
	// Country is the account country reported by the accesspoint; empty until
	// a session exists.
	Country string
	// Product is the account product type from the accesspoint ("premium",
	// "free", ...); empty until known.
	Product string
	// LastErrorCode is the protocol error code classifying LastError, if any.
	LastErrorCode string
}

// ConnectError is a session that could not be created. It is reported to
// protocol clients with the "unavailable" code, and it is never permanent:
// the manager keeps retrying in the background.
type ConnectError struct {
	Err error
}

func (e *ConnectError) Error() string { return "connecting to Spotify failed: " + e.Err.Error() }
func (e *ConnectError) Unwrap() error { return e.Err }

// IsConnectError reports whether err (or an error it wraps) means the Spotify
// session could not be established.
func IsConnectError(err error) bool {
	var ce *ConnectError
	return errors.As(err, &ce)
}

// sessionOpener creates one session generation. It is a field so tests can
// substitute a local stand-in for Spotify.
type sessionOpener func(ctx context.Context, c *creds.Credentials) (*Env, string, error)

// attempt is one session-creation round every waiting request shares.
type attempt struct {
	done chan struct{}
	env  *Env
	err  error
}

// Manager owns the shared Spotify session generation.
type Manager struct {
	log *logging.Logger

	// fixedCreds, when set, is used instead of the on-disk credentials file.
	fixedCreds *creds.Credentials

	ctx    context.Context
	cancel context.CancelFunc

	// opMu is held for reading while an operation uses the current session and
	// for writing while it is replaced or closed. Streaming keeps working on
	// the decoder it was handed, so it does not hold the lock.
	opMu sync.RWMutex

	mu       sync.Mutex
	started  bool
	closed   bool
	creds    *creds.Credentials
	loggedIn bool
	state    string
	lastErr  string
	// lastFailure is the last failed creation, returned to requests that arrive
	// while no new attempt is in flight.
	lastFailure error
	user        string
	env         *Env
	current     *attempt
	kick        chan struct{}
	invalidate  chan struct{}
	lastCC      atomic.Value // string
	product     atomic.Value // string: account product type ("premium", "free", ...)
	cacheHits   atomic.Int64 // streams served from the audio cache
	cacheDir    string
	cacheMB     int
	audioCache  *cache.Cache
	lastErrCode string
	openSession sessionOpener

	// streamMu serialises player.NewStream: its CDN quarantine map is not
	// concurrency safe.
	streamMu sync.Mutex

	// Metadata caches. Everything stored is immutable once built, so values
	// can be shared between requests.
	trackMeta   *lru[string, *trackMeta]
	trackInfos  *lru[trackInfoKey, TrackInfo]
	items       *lru[string, Item]
	artistPages *ttlLRU[string, *ArtistPage]

	libMu       sync.Mutex
	libSections []Section
	libAt       time.Time
}

// NewManager returns a manager bound to ctx. Nothing is created until Start
// (or the first request), so constructing one is free.
func NewManager(ctx context.Context, log *logging.Logger) *Manager {
	ctx, cancel := context.WithCancel(ctx)
	m := &Manager{
		log:        log,
		ctx:        ctx,
		cancel:     cancel,
		state:      StateConnecting,
		kick:       make(chan struct{}, 1),
		invalidate: make(chan struct{}),

		trackMeta:   newLRU[string, *trackMeta](20000),
		trackInfos:  newLRU[trackInfoKey, TrackInfo](20000),
		items:       newLRU[string, Item](5000),
		artistPages: newTTLLRU[string, *ArtistPage](artistCacheSize, artistCacheTTL),
	}
	m.openSession = m.openSessionFromSpotify
	return m
}

// NewManagerWithCredentials returns a manager that always uses c instead of
// reading the credentials file, which keeps the phase-0 spike on the same code
// path without changing where it stores its login.
func NewManagerWithCredentials(ctx context.Context, log *logging.Logger, c *creds.Credentials) *Manager {
	m := NewManager(ctx, log)
	m.fixedCreds = c
	return m
}

// NewManagerForTest returns a manager that creates its session with open.
// It exists so the bridge package can test status and request behaviour
// without a Spotify account.
func NewManagerForTest(ctx context.Context, log *logging.Logger, c *creds.Credentials,
	open func(context.Context, *creds.Credentials) (*Env, string, error)) *Manager {
	m := NewManagerWithCredentials(ctx, log, c)
	m.openSession = open
	return m
}

// Start begins creating the Spotify session in the background. It is
// idempotent, and requests call it too, so a manager is usable without an
// explicit Start.
func (m *Manager) Start() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.startLocked()
}

func (m *Manager) startLocked() {
	if m.started || m.closed {
		return
	}
	m.started = true

	c := m.fixedCreds
	if c == nil {
		var err error
		c, err = creds.Load()
		if err != nil {
			m.loggedIn = false
			m.state = StateError
			m.lastErr = "not logged in"
			m.log.Errorf("not logged in: %v", err)
			return
		}
	}

	m.creds = c
	m.loggedIn = true
	m.user = c.Username
	m.state = StateConnecting
	m.log.Infof("starting Spotify session in the background as %s", c.Username)
	go m.connectLoop()
}

// Status returns the current snapshot immediately; it never waits for a
// session.
func (m *Manager) Status() Status {
	m.mu.Lock()
	defer m.mu.Unlock()
	return Status{
		LoggedIn:  m.loggedIn,
		State:     m.state,
		LastError: m.lastErr,
		Username:  m.user,
		Country:   m.Country(),

		Product:       m.Product(),
		LastErrorCode: m.lastErrCode,
	}
}

// Product is the account's product type, or "" until the accesspoint said.
func (m *Manager) Product() string {
	if v, ok := m.product.Load().(string); ok {
		return v
	}
	return ""
}

// Country is the account's country code, or "" until the accesspoint said.
func (m *Manager) Country() string {
	if v, ok := m.lastCC.Load().(string); ok {
		return v
	}
	return ""
}

// Close releases the current session and stops creating new ones.
func (m *Manager) Close() {
	m.cancel()

	m.opMu.Lock()
	defer m.opMu.Unlock()
	m.mu.Lock()
	m.closed = true
	env := m.env
	m.env = nil
	m.current = nil
	m.state = StateError
	m.lastErr = "bridge is shutting down"
	m.mu.Unlock()

	if env != nil {
		closeEnv(env)
	}
}

// connectLoop keeps one session generation alive: create it, wait until it is
// invalidated, then create the next one. Failed rounds retry with backoff.
func (m *Manager) connectLoop() {
	backoff := retryBase
	for {
		if m.ctx.Err() != nil {
			return
		}

		m.mu.Lock()
		invalidate := m.invalidate
		m.mu.Unlock()

		a := m.beginAttempt()
		if a == nil {
			// Close raced with the start of this round.
			return
		}
		env, user, err := m.openSession(m.ctx, m.creds)
		m.finishAttempt(a, env, user, err)

		if err == nil {
			backoff = retryBase
			select {
			case <-m.ctx.Done():
				return
			case <-invalidate:
				m.log.Infof("Spotify session was invalidated; reconnecting")
			}
			continue
		}

		select {
		case <-m.ctx.Done():
			return
		case <-time.After(backoff):
		case <-m.kick:
		}
		backoff *= 2
		if backoff > retryMax {
			backoff = retryMax
		}
	}
}

func (m *Manager) beginAttempt() *attempt {
	a := &attempt{done: make(chan struct{})}
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return nil
	}
	m.current = a
	m.state = StateConnecting
	m.mu.Unlock()
	return a
}

func (m *Manager) finishAttempt(a *attempt, env *Env, user string, err error) {
	var failure error
	if err != nil {
		failure = err
		if !IsConnectError(failure) {
			failure = &ConnectError{Err: failure}
		}
	}

	m.opMu.Lock()
	m.mu.Lock()
	if m.current == a {
		m.current = nil
	}
	closed := m.closed
	if failure == nil && !closed {
		m.env = env
		if user != "" {
			m.user = user
		}
		m.state = StateConnected
		m.lastErr = ""
		m.lastErrCode = ""
		m.lastFailure = nil
	} else {
		if failure == nil {
			failure = &ConnectError{Err: errors.New("bridge is shutting down")}
		}
		m.state = StateError
		m.lastErr = failure.Error()
		m.lastErrCode = ""
		if coded := Classify(failure, m.Country(), m.Product()); coded != nil {
			m.lastErrCode = coded.Code
		}
		m.lastFailure = failure
	}
	m.mu.Unlock()
	m.opMu.Unlock()

	a.err = failure
	a.env = nil
	if failure == nil {
		a.env = env
		m.log.Infof("Spotify session ready as %s", user)
	} else {
		m.log.WithError(failure).Warnf("Spotify session creation failed")
		if env != nil {
			closeEnv(env)
		}
	}
	close(a.done)
}

func (m *Manager) kickConnector() {
	select {
	case m.kick <- struct{}{}:
	default:
	}
}

// wait returns the connected session, waiting for the in-flight creation (or
// the next retry) at most as long as the caller's ctx allows. Creation itself
// runs on the manager's own context, so a client giving up never cancels it.
func (m *Manager) wait(ctx context.Context) (*Env, error) {
	m.mu.Lock()
	started := m.started
	m.mu.Unlock()
	if !started {
		m.Start()
	}

	for {
		m.mu.Lock()
		if m.env != nil {
			env := m.env
			m.mu.Unlock()
			return env, nil
		}
		if !m.loggedIn {
			msg := m.lastErr
			m.mu.Unlock()
			if msg == "" {
				msg = "no stored credentials"
			}
			return nil, fmt.Errorf("%w (%s)", ErrNotLoggedIn, msg)
		}
		a := m.current
		lastFailure := m.lastFailure
		m.mu.Unlock()

		if a == nil {
			if lastFailure != nil {
				// No creation is in flight: report the last failure instead of
				// silently waiting out the whole retry backoff.
				return nil, lastFailure
			}
			m.kickConnector()
			select {
			case <-ctx.Done():
				return nil, ctx.Err()
			case <-time.After(20 * time.Millisecond):
			}
			continue
		}

		select {
		case <-a.done:
			if a.err != nil {
				return nil, a.err
			}
			if a.env == nil {
				return nil, &ConnectError{Err: errors.New("session creation returned no session")}
			}
			return a.env, nil
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}
}

// Do runs fn with the current session and, when fn fails with something that
// looks like a dead session, recreates it once and retries.
//
// Failures that map to a specific protocol code (Premium needed, rate limited,
// region, unavailable track) come back as *CodedError.
func (m *Manager) Do(ctx context.Context, fn func(*Env) error) error {
	err := m.do(ctx, fn)
	if err == nil {
		return nil
	}
	if coded := Classify(err, m.Country(), m.Product()); coded != nil {
		m.log.Debugf("classified %q as %s", err.Error(), coded.Code)
		return coded
	}
	return err
}

func (m *Manager) do(ctx context.Context, fn func(*Env) error) error {
	env, err := m.wait(ctx)
	if err != nil {
		return err
	}

	err = m.run(ctx, env, fn)
	if err == nil || IsPermanent(err) || ctx.Err() != nil {
		return err
	}
	if IsConnectError(err) {
		return err
	}
	m.log.WithError(err).Warnf("request failed, recreating the Spotify session and retrying")
	m.reset(env)

	env, err = m.wait(ctx)
	if err != nil {
		return err
	}
	return m.run(ctx, env, fn)
}

// run calls fn on env while holding the read lock that reset needs. If another
// goroutine replaced the generation in the meantime, it retries with the new
// one instead of touching a closed session.
func (m *Manager) run(ctx context.Context, env *Env, fn func(*Env) error) error {
	for {
		m.opMu.RLock()
		m.mu.Lock()
		current := m.env
		m.mu.Unlock()
		if current != env {
			m.opMu.RUnlock()
			next, err := m.wait(ctx)
			if err != nil {
				return err
			}
			env = next
			continue
		}
		err := fn(env)
		m.opMu.RUnlock()
		return err
	}
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
	var coded *CodedError
	if errors.As(err, &coded) {
		return true
	}
	var limited *spclient.RateLimitedError
	if errors.As(err, &limited) {
		return true
	}
	var keyErr *audio.KeyProviderError
	if errors.As(err, &keyErr) {
		return true
	}
	if IsUnsupported(err) || IsBadInput(err) {
		return true
	}
	var fe *finalError
	if errors.As(err, &fe) {
		return true
	}
	var cre *spclient.ContextResolveError
	if errors.As(err, &cre) && cre.IsAccessDenied() {
		return true
	}
	// A 4xx from the backend is the backend's final answer (a missing playlist,
	// a refused collection set); recreating the session would not change it.
	var statusErr *spclient.StatusError
	if errors.As(err, &statusErr) && isClientStatus(statusErr.StatusCode) {
		return true
	}
	var httpErr *librespot.HTTPStatusError
	if errors.As(err, &httpErr) && isClientStatus(httpErr.StatusCode) {
		return true
	}
	return false
}

func isClientStatus(status int) bool {
	return status >= 400 && status < 500
}

// reset closes the given generation so the connector builds the next one.
func (m *Manager) reset(env *Env) {
	m.opMu.Lock()
	m.mu.Lock()
	if m.env != env {
		m.mu.Unlock()
		m.opMu.Unlock()
		return
	}
	m.env = nil
	m.state = StateConnecting
	m.lastErr = ""
	m.lastFailure = nil
	close(m.invalidate)
	m.invalidate = make(chan struct{})
	m.mu.Unlock()

	closeEnv(env)
	m.opMu.Unlock()
	m.log.Infof("Spotify session closed")
}

// openSessionFromSpotify creates one session generation from stored
// credentials, trying a few accesspoints with a short deadline each.
func (m *Manager) openSessionFromSpotify(ctx context.Context, c *creds.Credentials) (*Env, string, error) {
	client := librespot.NewHttpClient()
	// Prefer the ports a restrictive network can be expected to allow; the
	// dedicated 4070 accesspoints come last, and a dead one costs one short
	// attempt instead of the library's full handshake timeout.
	resolver := apresolve.NewApResolver(m.log, client, apresolve.AccesspointPorts(nil, true))

	opts := &session.Options{
		Log:         m.log,
		DeviceType:  devicespb.DeviceType(c.DeviceType),
		DeviceId:    c.DeviceID,
		Client:      client,
		Resolver:    resolver,
		Credentials: session.StoredCredentials{Username: c.Username, Data: c.Data},
	}

	var lastErr error
	for attempt := 1; attempt <= sessionAttempts; attempt++ {
		if err := ctx.Err(); err != nil {
			return nil, "", err
		}

		start := time.Now()
		actx, cancel := context.WithTimeout(ctx, sessionAttemptTimeout)
		sess, err := session.NewSessionFromOptions(actx, opts)
		cancel()
		if err != nil {
			lastErr = err
			m.log.WithError(err).Warnf("accesspoint attempt %d/%d failed after %s",
				attempt, sessionAttempts, time.Since(start).Round(time.Millisecond))
			continue
		}

		p, err := m.newPlayer(sess)
		if err != nil {
			sess.Close()
			return nil, "", fmt.Errorf("creating player: %w", err)
		}

		// The accesspoint reports the account country right after
		// authentication; the player needs it to apply media restrictions and
		// alternative-track relinking. Keep draining so a reconnect's packet
		// cannot block the reader.
		go m.trackCountry(sess)
		go m.trackProduct(sess)

		m.log.Infof("Spotify session created in %s (username %s)",
			time.Since(start).Round(time.Millisecond), sess.Username())
		return &Env{Session: sess, Player: p}, sess.Username(), nil
	}

	if lastErr == nil {
		lastErr = errors.New("no accesspoint answered")
	}
	return nil, "", lastErr
}

func (m *Manager) trackCountry(sess *session.Session) {
	ch := sess.Accesspoint().Receive(ap.PacketTypeCountryCode)
	for pkt := range ch {
		if code := string(pkt.Payload); code != "" {
			m.lastCC.Store(code)
		}
	}
}

// NewStream opens a decoded stream, serialising the player call because the
// library's CDN bookkeeping is not concurrency safe.
func (m *Manager) NewStream(ctx context.Context, env *Env, id librespot.SpotifyId, bitrate int, posMs int64) (*player.Stream, error) {
	m.streamMu.Lock()
	defer m.streamMu.Unlock()
	return env.Player.NewStream(ctx, librespot.NewHttpClient(), id, bitrate, posMs)
}

// closeEnv releases a session generation. Nil parts are tolerated so tests can
// use an empty Env.
func closeEnv(env *Env) {
	if env == nil {
		return
	}
	if env.Player != nil {
		env.Player.Close()
	}
	if env.Session != nil {
		env.Session.Close()
	}
}
