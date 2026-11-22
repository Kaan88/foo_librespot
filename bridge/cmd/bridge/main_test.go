package main

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"strings"
	"sync"
	"testing"
	"time"

	"foo_librespot/bridge/internal/bridge"
	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/spotifyx"
)

// syncBuffer is a log sink safe for the watcher goroutine.
type syncBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *syncBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *syncBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

func startWatch(t *testing.T, pid int) (<-chan struct{}, *syncBuffer, context.CancelFunc) {
	t.Helper()
	ctx, cancelCtx := context.WithCancel(context.Background())
	fired := make(chan struct{})
	var once sync.Once
	out := &syncBuffer{}
	log := logging.New(out, false).WithPrefix("test")
	go watchParent(ctx, pid, func() { once.Do(func() { close(fired) }) }, log)
	t.Cleanup(cancelCtx)
	return fired, out, cancelCtx
}

func TestWatchParentIgnoresLiveParent(t *testing.T) {
	fired, out, _ := startWatch(t, os.Getpid())
	select {
	case <-fired:
		t.Fatalf("watcher fired for a live process; log:\n%s", out.String())
	case <-time.After(1500 * time.Millisecond):
	}
	if !strings.Contains(out.String(), "watching parent process") {
		t.Fatalf("watcher did not start; log:\n%s", out.String())
	}
}

func TestWatchParentFiresWhenParentExits(t *testing.T) {
	cmd := exec.Command(os.Getenv("ComSpec"), "/c", "ping", "-n", "30", "127.0.0.1")
	if err := cmd.Start(); err != nil {
		t.Skipf("cannot start a helper process: %v", err)
	}
	pid := cmd.Process.Pid
	defer func() {
		_ = cmd.Process.Kill()
		_, _ = cmd.Process.Wait()
	}()

	fired, out, _ := startWatch(t, pid)
	select {
	case <-fired:
		t.Fatal("watcher fired while the parent was alive")
	case <-time.After(1200 * time.Millisecond):
	}

	if err := cmd.Process.Kill(); err != nil {
		t.Fatalf("killing the parent: %v", err)
	}
	_, _ = cmd.Process.Wait()

	select {
	case <-fired:
	case <-time.After(5 * time.Second):
		t.Fatalf("watcher did not fire after the parent died; log:\n%s", out.String())
	}
	log := out.String()
	if !strings.Contains(log, "exited (exit code") {
		t.Fatalf("log does not name the exit code; log:\n%s", log)
	}
}

func TestWatchParentFiresForMissingPid(t *testing.T) {
	cmd := exec.Command(os.Getenv("ComSpec"), "/c", "exit", "0")
	if err := cmd.Run(); err != nil {
		t.Fatalf("helper process: %v", err)
	}
	fired, out, _ := startWatch(t, cmd.Process.Pid)
	select {
	case <-fired:
	case <-time.After(5 * time.Second):
		t.Fatalf("watcher did not fire for a dead pid; log:\n%s", out.String())
	}
}

func TestPipeAliveReportsOnlyRealServers(t *testing.T) {
	const name = "foo_librespot_pipe_probe_test"
	path := `\\.\pipe\` + name
	if pipeAlive(path, 100*time.Millisecond) {
		t.Fatalf("pipeAlive found a server on an unused pipe")
	}

	cancel := startTestServer(t, name)
	defer cancel()
	waitForPipe(t, path)

	if !pipeAlive(path, time.Second) {
		t.Fatalf("pipeAlive did not see the running server")
	}
}

func startTestServer(t *testing.T, name string) context.CancelFunc {
	t.Helper()
	t.Setenv("APPDATA", t.TempDir()) // a manager with no stored credentials
	ctx, cancel := context.WithCancel(context.Background())
	log := logging.New(io.Discard, false)
	mgr := spotifyx.NewManager(ctx, log)
	srv := bridge.NewServer(ctx, log, mgr)
	go func() { _ = srv.ListenAndServe(name) }()
	t.Cleanup(func() {
		cancel()
		mgr.Close()
	})
	return cancel
}

func waitForPipe(t *testing.T, path string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !pipeAlive(path, 100*time.Millisecond) {
		if time.Now().After(deadline) {
			t.Fatalf("server never listened on %s", path)
		}
		time.Sleep(20 * time.Millisecond)
	}
}
