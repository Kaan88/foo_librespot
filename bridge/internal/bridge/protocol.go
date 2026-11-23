// Package bridge implements the named-pipe protocol described in
// proto/PROTOCOL.md.
package bridge

import (
	"errors"

	"foo_librespot/bridge/internal/spotifyx"
)

// Version is the bridge implementation version reported by status.
const Version = "0.3.0"

// Error codes of the protocol.
const (
	CodeNotLoggedIn = "not_logged_in"
	CodeUnavailable = "unavailable"
	CodeBadRequest  = "bad_request"
	CodeInternal    = "internal"
	CodeUnsupported = "unsupported"

	// Phase 5: specific failures (see spotifyx.Classify).
	CodePremiumRequired   = spotifyx.CodePremiumRequired
	CodeRateLimited       = spotifyx.CodeRateLimited
	CodeRegionUnavailable = spotifyx.CodeRegionUnavailable
	CodeTrackUnavailable  = spotifyx.CodeTrackUnavailable
)

// request is one JSON line from a client.
type request struct {
	Op      string   `json:"op"`
	URI     string   `json:"uri"`
	URIs    []string `json:"uris"`
	StartMs *int64   `json:"start_ms"`
	Bitrate *int     `json:"bitrate"`
	Query   string   `json:"query"`
	Limit   *int     `json:"limit"`
	// Playlist and DryRun belong to playlist_add.
	Playlist string `json:"playlist"`
	DryRun   bool   `json:"dry_run"`
	// CacheMB and ClearCache belong to config.
	CacheMB    *int `json:"cache_mb"`
	ClearCache bool `json:"clear_cache"`
}

type statusResponse struct {
	OK        bool   `json:"ok"`
	LoggedIn  bool   `json:"logged_in"`
	Username  string `json:"username"`
	Country   string `json:"country"`
	State     string `json:"state"`
	LastError string `json:"last_error"`
	Version   string `json:"version"`
	// Optional (v4): account product type and the error code of last_error.
	Product       string `json:"product,omitempty"`
	LastErrorCode string `json:"last_error_code,omitempty"`
}

// configResponse answers the config op with the cache state.
type configResponse struct {
	OK         bool   `json:"ok"`
	CacheMB    int    `json:"cache_mb"`
	CacheDir   string `json:"cache_dir"`
	CacheBytes int64  `json:"cache_bytes"`
}

type tracksResponse struct {
	OK     bool                 `json:"ok"`
	Tracks []spotifyx.TrackInfo `json:"tracks"`
}

// resolveResponse is tracks plus, for album and playlist contexts, the
// description of the context the tracks came from.
type resolveResponse struct {
	OK      bool                  `json:"ok"`
	Tracks  []spotifyx.TrackInfo  `json:"tracks"`
	Context *spotifyx.ContextInfo `json:"context,omitempty"`
}

type libraryResponse struct {
	OK       bool               `json:"ok"`
	Sections []spotifyx.Section `json:"sections"`
}

type searchResponse struct {
	OK        bool                 `json:"ok"`
	Tracks    []spotifyx.TrackInfo `json:"tracks"`
	Albums    []spotifyx.Item      `json:"albums"`
	Artists   []spotifyx.Item      `json:"artists"`
	Playlists []spotifyx.Item      `json:"playlists"`
}

type artistResponse struct {
	OK        bool                 `json:"ok"`
	Artist    spotifyx.Item        `json:"artist"`
	TopTracks []spotifyx.TrackInfo `json:"top_tracks"`
	Albums    []spotifyx.Item      `json:"albums"`
	Singles   []spotifyx.Item      `json:"singles"`
}

type savedResponse struct {
	OK    bool   `json:"ok"`
	Saved []bool `json:"saved"`
}

type okResponse struct {
	OK bool `json:"ok"`
}

type playlistAddResponse struct {
	OK       bool   `json:"ok"`
	Revision string `json:"revision"`
	// DryRun and Request are set when nothing was sent.
	DryRun  bool                      `json:"dry_run,omitempty"`
	Request *spotifyx.PlaylistRequest `json:"request,omitempty"`
}

type openHeader struct {
	OK         bool               `json:"ok"`
	Track      spotifyx.TrackInfo `json:"track"`
	SampleRate int32              `json:"sample_rate"`
	Channels   int32              `json:"channels"`
	StartFrame int64              `json:"start_frame"`
	// FromCache (v4, optional) is true when the encrypted audio came from the
	// on-disk audio cache instead of the network.
	FromCache bool `json:"from_cache,omitempty"`
}

type errorResponse struct {
	OK    bool   `json:"ok"`
	Error string `json:"error"`
	Code  string `json:"code"`
	// RetryAfter (v4, optional) is the advised wait in seconds for rate_limited.
	RetryAfter int `json:"retry_after,omitempty"`
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
	var ce *spotifyx.CodedError
	switch {
	case errors.As(err, &ae):
		return ae.code
	case errors.As(err, &ce):
		return ce.Code
	case spotifyx.IsBadInput(err):
		return CodeBadRequest
	case errors.Is(err, spotifyx.ErrNotLoggedIn):
		return CodeNotLoggedIn
	case spotifyx.IsConnectError(err):
		return CodeUnavailable
	case spotifyx.IsUnsupported(err):
		return CodeUnsupported
	case spotifyx.IsPermanent(err):
		return CodeUnavailable
	default:
		return CodeInternal
	}
}
