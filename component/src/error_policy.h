// How a bridge failure is presented to the user and to foobar2000.
//
// SDK-free (like sync_plan and metadata), so the classification is unit
// tested without a foobar2000 host.

#pragma once

#include "bridge/protocol.h"

#include <string>

namespace flp {

// What to do about one bridge failure.
struct error_info {
	// The protocol error code the failure maps to ("premium_required",
	// "rate_limited", "region_unavailable", "track_unavailable",
	// "not_logged_in", "connecting" or "other").
	std::string kind;
	// The short message shown to the user ("Spotify: ...").
	std::string message;
	// True when this track cannot be played but the next one may be: playback
	// should move on instead of stopping.
	bool skip_track = false;
	// True for the kinds worth a popup (once per kind and session).
	bool popup = false;
};

// Classifies a bridge reply by its code (and, for old bridges that report
// "unavailable" for everything, by its message).
error_info describe_error(const std::string& code, const std::string& detail, int retry_after);

// Same for any exception; non-bridge errors are kind "other".
error_info describe_error(const std::exception& error);

// True the first time it is called with `kind` in this process, false after
// that. Keeps a playlist of unplayable tracks from raising one popup each.
bool first_report_of_kind(const std::string& kind);

// Test hook: forgets every kind reported so far.
void reset_reported_kinds();

} // namespace flp
