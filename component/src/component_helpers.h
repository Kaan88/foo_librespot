#pragma once

// Component-side glue between the foobar2000 SDK and the bridge client:
// path setup, abort_callback bridging, exception translation and file_info
// population.

#include "bridge/protocol.h"
#include "metadata.h"

#include <SDK/file_info.h>

#include <stdexcept>
#include <string>

namespace flp {

// Sets the librespot-bridge.exe path from core_api::get_my_full_path() once.
void init_bridge_paths();

// Wraps a foobar2000 abort_callback for the bridge client.
cancel_token make_cancel(abort_callback& aborter);

// Makes sure the bridge process is up; throws a foobar2000 I/O exception on
// failure (including "not logged in", which is detected by the caller's first
// request rather than here).
void ensure_bridge_started(abort_callback& aborter);

// Translates bridge/pipe errors into foobar2000 I/O exceptions, printing the
// details to the console first. Never returns.
[[noreturn]] void throw_io_error(const std::exception& error);

// Parses "spotify:track:<id>" paths.
bool is_spotify_track_path(const char* path);
bool is_spotify_track_uri(const std::string& uri);

// Canonical playlist path for a resolved track.
std::string track_path(const track_info& track);

} // namespace flp
