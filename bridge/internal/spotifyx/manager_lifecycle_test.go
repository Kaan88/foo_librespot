package spotifyx

import (
	"context"
	"errors"
	"io"
	"sync"
	"testing"
	"time"

	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
)

func testCreds() *creds.Credentials {
	return &creds.Credentials{
		Version:  1,
		DeviceID: "0123456789abcdef0123456789abcdef01234567",
		Username: "tester",
		Data:     []byte{1},
		SavedAt:  time.Now(),
	}
}

func testLog() *logging.Logger { return logging.New(io.Discard, false) }

func TestStatusIsImmediateWhileConnecting(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	m := NewManagerForTest(context.Background(), testLog(), testCreds(),
		func(ctx context.Context, c *creds.Credentials) (*Env, string, error) {
			close(started)
			select {
			case <-release:
				return &Env{}, "tester", nil
			case <-ctx.Done():
				return nil, "", ctx.Err()
			}
		})
	defer m.Close()
	m.Start()
	<-started

	begin := time.Now()
	st := m.Status()
	if elapsed := time.Since(begin); elapsed > 50*time.Millisecond {
		t.Fatalf("status took %s", elapsed)
	}
	if !st.LoggedIn || st.State != StateConnecting || st.Username != "tester" {
		t.Fatalf("unexpected status %+v", st)
	}

	close(release)
	deadline := time.Now().Add(2 * time.Second)
	for m.Status().State != StateConnected {
		if time.Now().After(deadline) {
			t.Fatalf("never connected: %+v", m.Status())
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestClientGivingUpDoesNotCancelCreation(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	var mu sync.Mutex
	opens := 0
	m := NewManagerForTest(context.Background(), testLog(), testCreds(),
		func(ctx context.Context, c *creds.Credentials) (*Env, string, error) {
			mu.Lock()
			opens++
			mu.Unlock()
			close(started)
			select {
			case <-release:
				return &Env{}, "tester", nil
			case <-ctx.Done():
				return nil, "", ctx.Err() // must never happen: this is the manager's ctx
			}
		})
	defer m.Close()
	m.Start()
	<-started

	// A request that gives up immediately must not poison the shared creation.
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if _, err := m.wait(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("wait error = %v, want deadline exceeded", err)
	}

	// A second request joins the same in-flight creation.
	got := make(chan error, 1)
	go func() {
		_, err := m.wait(context.Background())
		got <- err
	}()
	select {
	case err := <-got:
		t.Fatalf("second waiter returned early: %v", err)
	case <-time.After(50 * time.Millisecond):
	}
	close(release)
	if err := <-got; err != nil {
		t.Fatalf("second waiter failed: %v", err)
	}

	mu.Lock()
	defer mu.Unlock()
	if opens != 1 {
		t.Fatalf("session opened %d times, want 1 (single-flight)", opens)
	}
}

func TestConcurrentWaitersShareOneCreation(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	var mu sync.Mutex
	opens := 0
	m := NewManagerForTest(context.Background(), testLog(), testCreds(),
		func(ctx context.Context, c *creds.Credentials) (*Env, string, error) {
			mu.Lock()
			opens++
			mu.Unlock()
			select {
			case <-started:
			default:
				close(started)
			}
			<-release
			return &Env{}, "tester", nil
		})
	defer m.Close()
	m.Start()
	<-started

	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if _, err := m.wait(context.Background()); err != nil {
				t.Errorf("wait: %v", err)
			}
		}()
	}
	time.Sleep(50 * time.Millisecond)
	close(release)
	wg.Wait()

	mu.Lock()
	defer mu.Unlock()
	if opens != 1 {
		t.Fatalf("session opened %d times, want 1", opens)
	}
}

func TestFailedCreationFailsWaiters(t *testing.T) {
	var mu sync.Mutex
	opens := 0
	m := NewManagerForTest(context.Background(), testLog(), testCreds(),
		func(ctx context.Context, c *creds.Credentials) (*Env, string, error) {
			mu.Lock()
			opens++
			mu.Unlock()
			return nil, "", errors.New("no route to Spotify")
		})
	defer m.Close()
	m.Start()

	const want = "connecting to Spotify failed: no route to Spotify"

	// The first request may arrive while the first round is failing, or during
	// the pause before the retry; either way it must end in a clear failure.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := m.wait(ctx)
	if err == nil || !IsConnectError(err) {
		t.Fatalf("wait error = %v, want a ConnectError", err)
	}
	if err.Error() != want {
		t.Fatalf("error = %q, want %q", err, want)
	}
	st := m.Status()
	if st.State != StateError || st.LastError != want || !st.LoggedIn {
		t.Fatalf("status = %+v", st)
	}
}

func TestRetriesAfterFailure(t *testing.T) {
	var mu sync.Mutex
	opens := 0
	m := NewManagerForTest(context.Background(), testLog(), testCreds(),
		func(ctx context.Context, c *creds.Credentials) (*Env, string, error) {
			mu.Lock()
			opens++
			n := opens
			mu.Unlock()
			if n == 1 {
				return nil, "", errors.New("first round fails")
			}
			return &Env{}, "tester", nil
		})
	defer m.Close()
	m.Start()

	deadline := time.Now().Add(10 * time.Second)
	for {
		if st := m.Status(); st.State == StateConnected {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("never connected: %+v", m.Status())
		}
		time.Sleep(50 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	if opens < 2 {
		t.Fatalf("opened %d times, want a retry", opens)
	}
}

func TestNoCredentialsIsNotLoggedIn(t *testing.T) {
	t.Setenv("APPDATA", t.TempDir())
	m := NewManager(context.Background(), testLog())
	m.fixedCreds = nil
	m.Start()
	if st := m.Status(); st.LoggedIn || st.State != StateError {
		t.Fatalf("status = %+v", st)
	}
	if _, err := m.wait(context.Background()); !errors.Is(err, ErrNotLoggedIn) {
		t.Fatalf("wait error = %v, want ErrNotLoggedIn", err)
	}
}

func TestCloseDuringCreationIsClean(t *testing.T) {
	started := make(chan struct{})
	var once sync.Once
	m := NewManagerForTest(context.Background(), testLog(), testCreds(),
		func(ctx context.Context, c *creds.Credentials) (*Env, string, error) {
			once.Do(func() { close(started) })
			<-ctx.Done()
			return nil, "", ctx.Err()
		})
	m.Start()
	<-started

	done := make(chan struct{})
	go func() {
		m.Close()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("Close blocked behind a session creation")
	}
	if st := m.Status(); st.State != StateError {
		t.Fatalf("status after Close = %+v", st)
	}
}
