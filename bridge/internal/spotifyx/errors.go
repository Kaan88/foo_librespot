package spotifyx

import (
	"errors"
	"fmt"
	"net/http"
	"regexp"
	"strconv"
	"strings"

	librespot "github.com/devgianlu/go-librespot"
	"github.com/devgianlu/go-librespot/ap"
	"github.com/devgianlu/go-librespot/audio"
	pb "github.com/devgianlu/go-librespot/proto/spotify"
	"github.com/devgianlu/go-librespot/spclient"
)

// Protocol error codes that classification can produce (see PROTOCOL.md).
const (
	CodePremiumRequired   = "premium_required"
	CodeRateLimited       = "rate_limited"
	CodeRegionUnavailable = "region_unavailable"
	CodeTrackUnavailable  = "track_unavailable"
	CodeNotLoggedIn       = "not_logged_in"
)

// CodedError is a Spotify/librespot failure mapped to a protocol error code
// and a message meant for the user. The original error stays reachable
// through Unwrap, so the permanence checks keep working.
type CodedError struct {
	Code string
	Msg  string
	// RetryAfter is the advised wait in seconds for CodeRateLimited; 0 means
	// Spotify did not say.
	RetryAfter int
	Err        error
}

func (e *CodedError) Error() string { return e.Msg }
func (e *CodedError) Unwrap() error { return e.Err }

// IsFreeProduct reports whether an account product type from the accesspoint's
// product info means no Premium ("free", "open", ...). An unknown (empty)
// product is not treated as free.
func IsFreeProduct(product string) bool {
	product = strings.ToLower(strings.TrimSpace(product))
	return product != "" && product != "premium"
}

// Classify maps err to a coded error, or returns nil when err is not one of
// the failures with a specific message. country and product are what is known
// about the account (either may be empty) and only refine the message/code.
func Classify(err error, country, product string) *CodedError {
	if err == nil {
		return nil
	}
	var already *CodedError
	if errors.As(err, &already) {
		return already
	}

	wrap := func(code, msg string, retry int) *CodedError {
		return &CodedError{Code: code, Msg: msg, RetryAfter: retry, Err: err}
	}
	premium := func() *CodedError {
		return wrap(CodePremiumRequired,
			"Spotify Premium is required: this account is not Premium, or its Premium subscription has expired", 0)
	}

	// Login refused by the accesspoint.
	var loginErr *ap.AccesspointLoginError
	if errors.As(err, &loginErr) && loginErr.Message != nil {
		switch loginErr.Message.GetErrorCode() {
		case pb.ErrorCode_PremiumAccountRequired:
			return premium()
		case pb.ErrorCode_BadCredentials, pb.ErrorCode_CouldNotValidateCredentials:
			return wrap(CodeNotLoggedIn, "Spotify rejected the saved login; run the login again", 0)
		}
		return nil
	}

	// The track is restricted in the account's country, and relinking found no
	// playable alternative.
	if errors.Is(err, librespot.ErrMediaRestricted) {
		msg := "This track is not available in your country"
		if country != "" {
			msg = fmt.Sprintf("This track is not available in your country (%s)", country)
		}
		return wrap(CodeRegionUnavailable, msg, 0)
	}

	if errors.Is(err, librespot.ErrNoSupportedFormats) {
		if IsFreeProduct(product) {
			return premium()
		}
		return wrap(CodeTrackUnavailable, "This track has no playable audio file on Spotify", 0)
	}

	// Rate limiting: 429 from any Spotify endpoint.
	var limited *spclient.RateLimitedError
	if errors.As(err, &limited) {
		secs := int(limited.RetryAfter.Seconds() + 0.5)
		return wrap(CodeRateLimited, rateLimitedMessage(secs), secs)
	}
	if status, ok := statusOf(err); ok {
		switch {
		case status == http.StatusTooManyRequests:
			// The library's HTTPStatusError does not keep the Retry-After header.
			return wrap(CodeRateLimited, rateLimitedMessage(0), 0)
		case status == http.StatusNotFound || status == http.StatusGone:
			return wrap(CodeTrackUnavailable, "This track is no longer available on Spotify", 0)
		case status == http.StatusForbidden || status == http.StatusUnauthorized:
			if IsFreeProduct(product) {
				return premium()
			}
			return wrap(CodeTrackUnavailable, "Spotify refused access to this track", 0)
		}
		return nil
	}

	// The audio key was refused: Spotify does that for non-Premium accounts and
	// for tracks that cannot be played at all.
	var keyErr *audio.KeyProviderError
	if errors.As(err, &keyErr) {
		if IsFreeProduct(product) {
			return premium()
		}
		return wrap(CodeTrackUnavailable,
			fmt.Sprintf("Spotify refused the decryption key for this track (code %d)", keyErr.Code), 0)
	}
	var keyVal audio.KeyProviderError
	if errors.As(err, &keyVal) {
		if IsFreeProduct(product) {
			return premium()
		}
		return wrap(CodeTrackUnavailable,
			fmt.Sprintf("Spotify refused the decryption key for this track (code %d)", keyVal.Code), 0)
	}

	return nil
}

func rateLimitedMessage(retryAfter int) string {
	if retryAfter > 0 {
		return fmt.Sprintf("Spotify is rate limiting requests; try again in %d seconds", retryAfter)
	}
	return "Spotify is rate limiting requests; try again in a minute"
}

var metadataStatusRe = regexp.MustCompile(`extended metadata request returned status (\d+)`)

// statusOf extracts an HTTP status from the library's status error types, and
// from the one place that only formats it into text (the extended-metadata
// batch entries).
func statusOf(err error) (int, bool) {
	if m := metadataStatusRe.FindStringSubmatch(err.Error()); m != nil {
		if status, convErr := strconv.Atoi(m[1]); convErr == nil {
			return status, true
		}
	}
	var httpErr *librespot.HTTPStatusError
	if errors.As(err, &httpErr) {
		return httpErr.StatusCode, true
	}
	var statusErr *spclient.StatusError
	if errors.As(err, &statusErr) {
		return statusErr.StatusCode, true
	}
	return 0, false
}
