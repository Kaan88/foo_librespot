// Package bridge implements the named-pipe protocol described in
// proto/PROTOCOL.md.
package bridge

import (
	"errors"

	"foo_librespot/bridge/internal/spotifyx"
)

// Version is the bridge implementation version reported by status.
const Version = "0.1.0"

// Error codes of the protocol.
const (
	CodeNotLoggedIn = "not_logged_in"
	CodeUnavailable = "unavailable"
	CodeBadRequest  = "bad_request"
	CodeInternal    = "internal"
)

// request is one JSON line from a client.
type request struct {
	Op      string   `json:"op"`
	URI     string   `json:"uri"`
	URIs    []string `json:"uris"`
	StartMs *int64   `json:"start_ms"`
	Bitrate *int     `json:"bitrate"`
}

type statusResponse struct {
	OK       bool   `json:"ok"`
	LoggedIn bool   `json:"logged_in"`
	Username string `json:"username"`
	Country  string `json:"country"`
	Version  string `json:"version"`
}

type tracksResponse struct {
	OK     bool                 `json:"ok"`
	Tracks []spotifyx.TrackInfo `json:"tracks"`
}

type openHeader struct {
	OK         bool               `json:"ok"`
	Track      spotifyx.TrackInfo `json:"track"`
	SampleRate int32              `json:"sample_rate"`
	Channels   int32              `json:"channels"`
	StartFrame int64              `json:"start_frame"`
}

type errorResponse struct {
	OK    bool   `json:"ok"`
	Error string `json:"error"`
	Code  string `json:"code"`
}

// apiError carries a protocol error code.
type apiError struct {
	code string
	msg  string
}

func (e *apiError) Error() string { return e.msg }

func badRequest(msg string) error { return &apiError{code: CodeBadRequest, msg: msg} }

// codeFor maps an error to a protocol error code.
func codeFor(err error) string {
	var ae *apiError
	switch {
	case errors.As(err, &ae):
		return ae.code
	case errors.Is(err, spotifyx.ErrNotLoggedIn):
		return CodeNotLoggedIn
	case spotifyx.IsPermanent(err):
		return CodeUnavailable
	default:
		return CodeInternal
	}
}
