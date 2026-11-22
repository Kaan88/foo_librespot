package bridge

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"net"
	"sync"
	"testing"
	"time"

	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/spotifyx"
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

type fakeSessions struct {
	mu      sync.Mutex
	opens   int
	started chan struct{}
	release chan struct{}
	err     error
}

func newFakeSessions() *fakeSessions {
	return &fakeSessions{started: make(chan struct{}), release: make(chan struct{})}
}

func (f *fakeSessions) open(ctx context.Context, c *creds.Credentials) (*spotifyx.Env, string, error) {
	f.mu.Lock()
	f.opens++
	if f.opens == 1 {
		close(f.started)
	}
	f.mu.Unlock()
	select {
	case <-f.release:
	case <-ctx.Done():
		return nil, "", ctx.Err()
	}
	if f.err != nil {
		return nil, "", f.err
	}
	return &spotifyx.Env{}, "tester", nil
}

func (f *fakeSessions) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.opens
}

// request sends one line to a fresh server connection and returns the reply.
func ask(t *testing.T, srv *Server, payload string) string {
	t.Helper()
	line, err := requestLine(srv, payload)
	if err != nil {
		t.Fatalf("request %s: %v", payload, err)
	}
	return line
}

func requestLine(srv *Server, payload string) (string, error) {
	client, server := net.Pipe()
	go srv.handle(server)
	defer client.Close()

	_ = client.SetDeadline(time.Now().Add(30 * time.Second))
	if _, err := client.Write([]byte(payload + "\n")); err != nil {
		return "", err
	}
	return bufio.NewReader(client).ReadString('\n')
}

func TestStatusAnswersImmediatelyWhileConnecting(t *testing.T) {
	fake := newFakeSessions()
	log := logging.New(io.Discard, false)
	mgr := spotifyx.NewManagerForTest(context.Background(), log, testCreds(), fake.open)
	defer mgr.Close()
	mgr.Start()
	<-fake.started

	srv := NewServer(context.Background(), log, mgr)

	begin := time.Now()
	line := ask(t, srv, `{"op":"status"}`)
	elapsed := time.Since(begin)
	if elapsed > 200*time.Millisecond {
		t.Fatalf("status took %s while connecting", elapsed)
	}

	var status statusResponse
	if err := json.Unmarshal([]byte(line), &status); err != nil {
		t.Fatalf("bad status reply %q: %v", line, err)
	}
	if !status.OK || !status.LoggedIn || status.State != spotifyx.StateConnecting ||
		status.Username != "tester" || status.Country != "" {
		t.Fatalf("unexpected status %+v", status)
	}
	close(fake.release)
}

func TestRequestsFailWithUnavailableWhenCreationFails(t *testing.T) {
	fake := newFakeSessions()
	fake.err = io.ErrUnexpectedEOF // stands in for a network failure
	log := logging.New(io.Discard, false)
	mgr := spotifyx.NewManagerForTest(context.Background(), log, testCreds(), fake.open)
	defer mgr.Close()
	mgr.Start()
	<-fake.started

	srv := NewServer(context.Background(), log, mgr)

	reply := make(chan string, 1)
	go func() {
		line, err := requestLine(srv, `{"op":"resolve","uri":"spotify:album:4yP0hdKOZPNshxUOjY0cZj"}`)
		if err != nil {
			line = "request error: " + err.Error()
		}
		reply <- line
	}()

	// Let a second request join the same in-flight creation.
	time.Sleep(50 * time.Millisecond)
	second := make(chan string, 1)
	go func() {
		line, err := requestLine(srv, `{"op":"info","uris":["spotify:track:0VjIjW4GlUZAMYd2vXMi3b"]}`)
		if err != nil {
			line = "request error: " + err.Error()
		}
		second <- line
	}()
	time.Sleep(50 * time.Millisecond)

	close(fake.release)

	for i := 0; i < 2; i++ {
		var line string
		select {
		case line = <-reply:
			reply = nil
		case line = <-second:
			second = nil
		case <-time.After(5 * time.Second):
			t.Fatal("no reply after the creation failed")
		}

		var errResp errorResponse
		if err := json.Unmarshal([]byte(line), &errResp); err != nil {
			t.Fatalf("bad reply %q: %v", line, err)
		}
		if errResp.OK || errResp.Code != CodeUnavailable {
			t.Fatalf("reply = %+v, want ok=false code=%s", errResp, CodeUnavailable)
		}
		const want = "connecting to Spotify failed: "
		if len(errResp.Error) < len(want) || errResp.Error[:len(want)] != want {
			t.Fatalf("error = %q, want prefix %q", errResp.Error, want)
		}
	}

	if got := fake.count(); got != 1 {
		t.Fatalf("session created %d times, want 1 (single-flight)", got)
	}
}

func TestStatusReportsErrorAfterFailedCreation(t *testing.T) {
	fake := newFakeSessions()
	fake.err = io.ErrUnexpectedEOF
	log := logging.New(io.Discard, false)
	mgr := spotifyx.NewManagerForTest(context.Background(), log, testCreds(), fake.open)
	defer mgr.Close()
	mgr.Start()
	<-fake.started
	close(fake.release)

	deadline := time.Now().Add(5 * time.Second)
	for mgr.Status().State != spotifyx.StateError {
		if time.Now().After(deadline) {
			t.Fatalf("status never reported the error: %+v", mgr.Status())
		}
		time.Sleep(10 * time.Millisecond)
	}
	st := mgr.Status()
	if !st.LoggedIn || st.State != spotifyx.StateError || st.LastError == "" {
		t.Fatalf("status = %+v", st)
	}

	srv := NewServer(context.Background(), log, mgr)
	line := ask(t, srv, `{"op":"status"}`)
	var status statusResponse
	if err := json.Unmarshal([]byte(line), &status); err != nil {
		t.Fatalf("bad status reply %q: %v", line, err)
	}
	if status.State != spotifyx.StateError || status.LastError != st.LastError {
		t.Fatalf("status reply = %+v", status)
	}
}
