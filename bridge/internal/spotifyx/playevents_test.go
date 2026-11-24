package spotifyx

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	eventsenderpb "github.com/devgianlu/go-librespot/proto/spotify/event_sender"
	"google.golang.org/protobuf/proto"

	"foo_librespot/bridge/internal/creds"
)

func TestBuildPlayEvent(t *testing.T) {
	r := PlayReport{
		URI:         "spotify:track:0VjIjW4GlUZAMYd2vXMi3b",
		StartedAt:   1700000000000,
		MsPlayed:    185000,
		DurationMs:  200040,
		ReasonStart: "clickrow",
		ReasonEnd:   "trackdone",
		AudioFormat: "vorbis 320",
	}
	if err := r.validate(); err != nil {
		t.Fatal(err)
	}
	if r.ContextURI != r.URI {
		t.Fatalf("a track without context is its own context, got %q", r.ContextURI)
	}

	seq := []byte("0123456789abcdef")
	req, err := buildPlayEvent(r, "0123456789abcdef0123456789abcdef01234567", seq, 7,
		[]byte("session-id-bytes"), []byte("playback-id-0001"), time.UnixMilli(1700000200000))
	if err != nil {
		t.Fatal(err)
	}
	if len(req.GetEvent()) != 1 {
		t.Fatalf("%d events", len(req.GetEvent()))
	}
	env := req.GetEvent()[0]
	if env.GetEventName() != "RawCoreStream" || env.GetSequenceNumber() != 7 || string(env.GetSequenceId()) != string(seq) {
		t.Fatalf("envelope %v", env)
	}

	names := map[string][]byte{}
	for _, f := range env.GetEventFragment() {
		names[f.GetName()] = f.GetData()
	}
	for _, want := range []string{"message", "context_time", "context_client_id", "context_installation_id",
		"context_device_desktop", "context_application_desktop"} {
		if _, ok := names[want]; !ok {
			t.Errorf("fragment %s missing", want)
		}
	}

	var stream eventsenderpb.RawCoreStream
	if err := proto.Unmarshal(names["message"], &stream); err != nil {
		t.Fatal(err)
	}
	if stream.GetContentUri() != r.URI || stream.GetPlayContext() != r.URI || stream.GetMsPlayed() != 185000 ||
		stream.GetReasonStart() != "clickrow" || stream.GetReasonEnd() != "trackdone" ||
		stream.GetPlaybackStartTime() != 1700000000000 || string(stream.GetPlaybackId()) != "playback-id-0001" {
		t.Fatalf("stream %v", &stream)
	}
	var when eventsenderpb.Time
	if err := proto.Unmarshal(names["context_time"], &when); err != nil || when.GetValue() != 1700000200000 {
		t.Fatalf("context_time %v %v", &when, err)
	}
}

func TestPlayReportDefaultsReasons(t *testing.T) {
	r := PlayReport{URI: "spotify:track:0VjIjW4GlUZAMYd2vXMi3b", StartedAt: 1}
	if err := r.validate(); err != nil {
		t.Fatal(err)
	}
	if r.ReasonStart != "unknown" || r.ReasonEnd != "unknown" {
		t.Fatalf("reasons %q %q", r.ReasonStart, r.ReasonEnd)
	}
}

func TestEventSequenceCounts(t *testing.T) {
	var s eventSequence
	id1, n1, sess1 := s.take()
	id2, n2, sess2 := s.take()
	if string(id1) != string(id2) || string(sess1) != string(sess2) || n1 != 1 || n2 != 2 || len(id1) != 16 {
		t.Fatalf("sequence %x/%d %x/%d", id1, n1, id2, n2)
	}
}

// writeTestCreds puts a credentials file into a private %APPDATA%, so the
// test never touches the real login.
func writeTestCreds(t *testing.T) {
	t.Helper()
	c := testCreds()
	if err := c.Save(); err != nil {
		t.Fatal(err)
	}
}

func TestManagerPicksUpLoginAndLogsOut(t *testing.T) {
	appdata := t.TempDir()
	t.Setenv("APPDATA", appdata)
	path, err := creds.Path()
	if err != nil || filepath.Dir(filepath.Dir(path)) != appdata {
		t.Fatalf("credentials path %q not below the test APPDATA (%v)", path, err)
	}

	opened := make(chan string, 4)
	m := NewManager(context.Background(), testLog())
	m.openSession = func(ctx context.Context, c *creds.Credentials) (*Env, string, error) {
		opened <- c.Username
		return &Env{}, c.Username, nil
	}
	defer m.Close()
	m.Start()
	if st := m.Status(); st.LoggedIn {
		t.Fatalf("logged in without a credentials file: %+v", st)
	}

	// "login" in another process writes the file; the running manager must
	// notice it without a restart.
	writeTestCreds(t)
	deadline := time.Now().Add(5 * time.Second)
	for !m.Status().LoggedIn {
		if time.Now().After(deadline) {
			t.Fatalf("credentials never picked up: %+v", m.Status())
		}
		time.Sleep(50 * time.Millisecond)
	}
	select {
	case user := <-opened:
		if user != "tester" {
			t.Fatalf("opened as %q", user)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no session was opened after the login")
	}
	for m.Status().State != StateConnected {
		if time.Now().After(deadline) {
			t.Fatalf("never connected: %+v", m.Status())
		}
		time.Sleep(10 * time.Millisecond)
	}

	if err := m.Logout(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("credentials file still there after logout: %v", err)
	}
	st := m.Status()
	if st.LoggedIn || st.Username != "" || st.State != StateError {
		t.Fatalf("status after logout %+v", st)
	}
	if _, err := m.wait(context.Background()); err == nil {
		t.Fatal("a request after logout got a session")
	}
}
