package logging

import (
	"bytes"
	"errors"
	"strings"
	"testing"
)

func TestLinesCarryTimestampAndPrefix(t *testing.T) {
	var out bytes.Buffer
	log := New(&out, false).WithPrefix("pid=4242")

	log.Infof("watching parent process 7")
	log.WithError(errors.New("boom")).Warnf("something failed")

	lines := strings.Split(strings.TrimSuffix(out.String(), "\n"), "\n")
	if len(lines) != 2 {
		t.Fatalf("got %d lines, want 2:\n%s", len(lines), out.String())
	}
	for _, line := range lines {
		if !strings.Contains(line, "[pid=4242]") {
			t.Fatalf("line has no pid prefix: %q", line)
		}
		if len(line) < len("2006-01-02 15:04:05.000") || line[4] != '-' || line[10] != ' ' {
			t.Fatalf("line has no timestamp: %q", line)
		}
	}
	if !strings.Contains(lines[0], "INFO: [pid=4242] watching parent process 7") {
		t.Fatalf("unexpected first line: %q", lines[0])
	}
	if !strings.Contains(lines[1], "WARN: [pid=4242] [err=boom] something failed") {
		t.Fatalf("unexpected second line: %q", lines[1])
	}
}

func TestDebugIsOptIn(t *testing.T) {
	var quiet, loud bytes.Buffer
	New(&quiet, false).Debugf("hidden")
	New(&loud, true).Debugf("shown")
	if quiet.Len() != 0 {
		t.Fatalf("debug leaked: %q", quiet.String())
	}
	if !strings.Contains(loud.String(), "DEBUG: shown") {
		t.Fatalf("debug missing: %q", loud.String())
	}
}
