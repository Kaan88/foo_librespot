package bridge

import (
	"bytes"
	"encoding/json"
	"fmt"
	"testing"
	"time"

	librespot "github.com/devgianlu/go-librespot"
	"github.com/devgianlu/go-librespot/spclient"

	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/spotifyx"
)

func TestCodeForSpecificFailures(t *testing.T) {
	cases := map[string]error{
		CodeRegionUnavailable: spotifyx.Classify(fmt.Errorf("x: %w", librespot.ErrMediaRestricted), "TR", ""),
		CodeRateLimited:       spotifyx.Classify(&spclient.RateLimitedError{RetryAfter: time.Second}, "", ""),
		CodeTrackUnavailable:  spotifyx.Classify(&librespot.HTTPStatusError{Endpoint: "extended metadata", StatusCode: 404}, "", ""),
		CodePremiumRequired:   spotifyx.Classify(librespot.ErrNoSupportedFormats, "", "free"),
	}
	for want, err := range cases {
		if got := codeFor(err); got != want {
			t.Errorf("codeFor = %q, want %q", got, want)
		}
	}
}

func TestErrorReplyCarriesRetryAfter(t *testing.T) {
	var out bytes.Buffer
	s := &Server{log: logging.New(&bytes.Buffer{}, false)}
	s.writeError(&out, spotifyx.Classify(&spclient.RateLimitedError{RetryAfter: 42 * time.Second}, "", ""))
	var resp map[string]any
	if err := json.Unmarshal(out.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	if resp["code"] != "rate_limited" || resp["retry_after"] != float64(42) || resp["ok"] != false {
		t.Errorf("reply = %s", out.String())
	}

	// Errors without a retry hint stay exactly as before.
	out.Reset()
	s.writeError(&out, badRequest("no"))
	if out.String() != `{"ok":false,"error":"no","code":"bad_request"}`+"\n" {
		t.Errorf("plain error = %s", out.String())
	}
}

func TestConfigRequestParsing(t *testing.T) {
	var req request
	if err := json.Unmarshal([]byte(`{"op":"config","cache_mb":1024,"clear_cache":true}`), &req); err != nil ||
		req.CacheMB == nil || *req.CacheMB != 1024 || !req.ClearCache {
		t.Errorf("request = %+v, %v", req, err)
	}
	raw, _ := json.Marshal(configResponse{OK: true, CacheMB: 1024, CacheDir: `C:\c`, CacheBytes: 5})
	if string(raw) != `{"ok":true,"cache_mb":1024,"cache_dir":"C:\\c","cache_bytes":5}` {
		t.Errorf("reply = %s", raw)
	}
}
