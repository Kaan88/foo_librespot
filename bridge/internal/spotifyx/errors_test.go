package spotifyx

import (
	"errors"
	"fmt"
	"testing"
	"time"

	librespot "github.com/devgianlu/go-librespot"
	"github.com/devgianlu/go-librespot/ap"
	"github.com/devgianlu/go-librespot/audio"
	pb "github.com/devgianlu/go-librespot/proto/spotify"
	"github.com/devgianlu/go-librespot/spclient"
)

func loginFailed(code pb.ErrorCode) error {
	return &ConnectError{Err: &ap.AccesspointLoginError{Message: &pb.APLoginFailed{ErrorCode: code.Enum()}}}
}

func TestClassify(t *testing.T) {
	tests := []struct {
		name    string
		err     error
		country string
		product string
		code    string // "" = not classified
		retry   int
	}{
		{"premium login failure", loginFailed(pb.ErrorCode_PremiumAccountRequired), "", "", CodePremiumRequired, 0},
		{"bad credentials", loginFailed(pb.ErrorCode_BadCredentials), "", "", CodeNotLoggedIn, 0},
		{"other login failure", loginFailed(pb.ErrorCode_TryAnotherAP), "", "", "", 0},
		{"restricted", fmt.Errorf("opening: %w", librespot.ErrMediaRestricted), "TR", "premium", CodeRegionUnavailable, 0},
		{"no formats", fmt.Errorf("x: %w", librespot.ErrNoSupportedFormats), "TR", "premium", CodeTrackUnavailable, 0},
		{"no formats on free", librespot.ErrNoSupportedFormats, "TR", "free", CodePremiumRequired, 0},
		{"429 storage resolve", fmt.Errorf("failed resolving track storage: %w",
			&librespot.HTTPStatusError{Endpoint: "storage resolve", StatusCode: 429}), "", "premium", CodeRateLimited, 0},
		{"429 with Retry-After", &spclient.RateLimitedError{RetryAfter: 42 * time.Second}, "", "", CodeRateLimited, 42},
		{"404 metadata", &librespot.HTTPStatusError{Endpoint: "extended metadata", StatusCode: 404}, "", "premium", CodeTrackUnavailable, 0},
		{"403 audio file premium", &librespot.HTTPStatusError{Endpoint: "audio file", StatusCode: 403}, "", "premium", CodeTrackUnavailable, 0},
		{"403 audio file free", &librespot.HTTPStatusError{Endpoint: "audio file", StatusCode: 403}, "", "free", CodePremiumRequired, 0},
		{"collection 429", &spclient.StatusError{Op: "collection write", StatusCode: 429}, "", "", CodeRateLimited, 0},
		{"404 metadata as text", fmt.Errorf("failed getting track metadata: %w", errors.New("extended metadata request returned status 404")), "", "premium", CodeTrackUnavailable, 0},
		{"429 metadata as text", errors.New("extended metadata request returned status 429"), "", "premium", CodeRateLimited, 0},
		{"503 stays generic", &librespot.HTTPStatusError{Endpoint: "storage resolve", StatusCode: 503}, "", "", "", 0},
		{"audio key refused", fmt.Errorf("failed retrieving audio key: %w", &audio.KeyProviderError{Code: 2}), "", "premium", CodeTrackUnavailable, 0},
		{"audio key refused free", fmt.Errorf("failed retrieving audio key: %w", audio.KeyProviderError{Code: 2}), "", "free", CodePremiumRequired, 0},
		{"plain error", errors.New("boom"), "", "", "", 0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := Classify(tc.err, tc.country, tc.product)
			if tc.code == "" {
				if got != nil {
					t.Fatalf("classified as %s (%q), want nil", got.Code, got.Msg)
				}
				return
			}
			if got == nil {
				t.Fatalf("not classified, want %s", tc.code)
			}
			if got.Code != tc.code || got.RetryAfter != tc.retry {
				t.Errorf("got %s retry %d (%q), want %s retry %d", got.Code, got.RetryAfter, got.Msg, tc.code, tc.retry)
			}
			if got.Err != tc.err {
				t.Errorf("original error lost")
			}
			if Classify(got, "", "") != got {
				t.Errorf("classifying twice must keep the first result")
			}
		})
	}
}

func TestClassifyMessages(t *testing.T) {
	got := Classify(librespot.ErrMediaRestricted, "DE", "")
	if got.Msg != "This track is not available in your country (DE)" {
		t.Errorf("region message = %q", got.Msg)
	}
	got = Classify(&spclient.RateLimitedError{RetryAfter: 7 * time.Second}, "", "")
	if got.Msg != "Spotify is rate limiting requests; try again in 7 seconds" {
		t.Errorf("rate message = %q", got.Msg)
	}
}

func TestCodedErrorIsPermanent(t *testing.T) {
	if !IsPermanent(Classify(librespot.ErrMediaRestricted, "", "")) {
		t.Error("a coded error is a definitive answer")
	}
	if !IsPermanent(&spclient.RateLimitedError{RetryAfter: time.Second}) {
		t.Error("rate limiting must not trigger a session rebuild")
	}
}

func TestParseProduct(t *testing.T) {
	xmlPayload := []byte(`<products><product><type>premium</type><head-files-url>x</head-files-url></product></products>`)
	if got := parseProduct(xmlPayload); got != "premium" {
		t.Errorf("product = %q", got)
	}
	if got := parseProduct([]byte(`<products><product><type>Open</type></product></products>`)); got != "open" || !IsFreeProduct(got) {
		t.Errorf("open product = %q", got)
	}
	if got := parseProduct([]byte("garbage")); got != "" || IsFreeProduct(got) {
		t.Errorf("garbage = %q", got)
	}
}
