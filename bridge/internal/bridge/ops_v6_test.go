package bridge

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/spotifyx"
)

func v6Server(t *testing.T, parentPID int, clientPID int) (*Server, *atomic.Int32) {
	t.Helper()
	fake := newFakeSessions()
	close(fake.release)
	log := logging.New(io.Discard, false)
	mgr := spotifyx.NewManagerForTest(context.Background(), log, testCreds(), fake.open)
	t.Cleanup(mgr.Close)
	srv := NewServer(context.Background(), log, mgr)
	var stops atomic.Int32
	srv.SetOwner(parentPID, func() { stops.Add(1) })
	srv.clientPID = func(net.Conn) (int, bool) { return clientPID, true }
	return srv, &stops
}

func TestShutdownOnlyForTheOwningProcess(t *testing.T) {
	// Another foobar2000 instance (pid 200) talks to the bridge of pid 100.
	srv, stops := v6Server(t, 100, 200)
	var reply errorResponse
	if err := json.Unmarshal([]byte(ask(t, srv, `{"op":"shutdown"}`)), &reply); err != nil {
		t.Fatal(err)
	}
	if reply.OK || reply.Code != CodeBadRequest || stops.Load() != 0 {
		t.Fatalf("non-owner shutdown: reply %+v, stops %d", reply, stops.Load())
	}

	// The owner may.
	srv, stops = v6Server(t, 100, 100)
	line := ask(t, srv, `{"op":"shutdown"}`)
	// The reply goes out before the hook runs.
	for deadline := time.Now().Add(2 * time.Second); stops.Load() == 0 && time.Now().Before(deadline); {
		time.Sleep(5 * time.Millisecond)
	}
	if strings.TrimSpace(line) != `{"ok":true}` || stops.Load() != 1 {
		t.Fatalf("owner shutdown: reply %s, stops %d", line, stops.Load())
	}
}

func TestShutdownWithoutOwnerHook(t *testing.T) {
	srv, _ := v6Server(t, 0, 0)
	srv.shutdown = nil
	var reply errorResponse
	_ = json.Unmarshal([]byte(ask(t, srv, `{"op":"shutdown"}`)), &reply)
	if reply.OK || reply.Code != CodeUnsupported {
		t.Fatalf("reply %+v", reply)
	}
}

func TestStatusReportsPIDs(t *testing.T) {
	srv, _ := v6Server(t, 4242, 0)
	var status statusResponse
	if err := json.Unmarshal([]byte(ask(t, srv, `{"op":"status"}`)), &status); err != nil {
		t.Fatal(err)
	}
	if status.ParentPID != 4242 || status.PID <= 0 || status.Debug {
		t.Fatalf("status %+v", status)
	}
}

func TestConfigSwitchesDebugLogging(t *testing.T) {
	srv, _ := v6Server(t, 0, 0)
	var reply configResponse
	_ = json.Unmarshal([]byte(ask(t, srv, `{"op":"config","debug":true}`)), &reply)
	if !reply.OK || !reply.Debug || !srv.log.DebugEnabled() {
		t.Fatalf("debug on: %+v", reply)
	}
	reply = configResponse{}
	_ = json.Unmarshal([]byte(ask(t, srv, `{"op":"config","debug":false}`)), &reply)
	if !reply.OK || reply.Debug || srv.log.DebugEnabled() {
		t.Fatalf("debug off: %+v", reply)
	}
}

func TestLogoutOp(t *testing.T) {
	srv, _ := v6Server(t, 0, 0)
	if line := ask(t, srv, `{"op":"logout"}`); strings.TrimSpace(line) != `{"ok":true}` {
		t.Fatalf("logout reply %s", line)
	}
	var status statusResponse
	_ = json.Unmarshal([]byte(ask(t, srv, `{"op":"status"}`)), &status)
	if status.LoggedIn || status.Username != "" {
		t.Fatalf("status after logout %+v", status)
	}
	var reply errorResponse
	_ = json.Unmarshal([]byte(ask(t, srv, `{"op":"liked"}`)), &reply)
	if reply.OK || reply.Code != CodeNotLoggedIn {
		t.Fatalf("liked after logout %+v", reply)
	}
}

func TestReportPlayRefusesBadInput(t *testing.T) {
	srv, _ := v6Server(t, 0, 0)
	for _, payload := range []string{
		`{"op":"report_play","uri":"spotify:album:4yP0hdKOZPNshxUOjY0cZj","started_at":1,"ms_played":1000}`,
		`{"op":"report_play","uri":"spotify:track:0VjIjW4GlUZAMYd2vXMi3b","ms_played":1000}`,
		`{"op":"report_play","uri":"spotify:track:0VjIjW4GlUZAMYd2vXMi3b","started_at":1,"ms_played":-5}`,
		`{"op":"report_play","uri":"spotify:track:0VjIjW4GlUZAMYd2vXMi3b","started_at":1,"reason_end":"bogus"}`,
	} {
		var reply errorResponse
		_ = json.Unmarshal([]byte(ask(t, srv, payload)), &reply)
		if reply.OK || reply.Code != CodeBadRequest {
			t.Errorf("%s: reply %+v", payload, reply)
		}
	}
}
