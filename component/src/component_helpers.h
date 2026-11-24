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

// A cancel token for background bridge calls that have no abort_callback of
// their own (menu actions, caches, play reports). It becomes aborting when
// foobar2000 quits (initquit::on_quit), so no worker keeps the process alive
// waiting on the bridge.
cancel_token app_cancel_token();
bool app_is_quitting();

// Makes sure the bridge process is up; throws a foobar2000 I/O exception on
// failure (including "not logged in", which is detected by the caller's first
// request rather than here).
void ensure_bridge_started(abort_callback& aborter);

// Translates bridge/pipe errors into foobar2000 I/O exceptions, printing the
// details to the console first. Never returns.
[[noreturn]] void throw_io_error(const std::exception& error);

// Like throw_io_error, for playing `track` (a path/URI shown in the console):
// also raises a popup, once per kind of failure and session. Unplayable tracks
// (track_unavailable / region_unavailable) throw the exception foobar2000
// treats as a per-item failure so playback moves on to the next track.
[[noreturn]] void throw_track_error(const std::exception& error, const char* track);

// A track Spotify lists as not playable (no usable audio file).
[[noreturn]] void throw_unplayable(const char* track);

// Applies the audio cache size to the bridge: spawns pick it up through
// --cache-mb; a bridge that is already running gets the `config` op, which
// applies it live (no restart). Returns true and fills `out` when a running
// bridge answered; false when none is running or it is too old for `config`
// (`error` is then empty / the reason).
bool apply_cache_setting(int cache_mb, bool clear_cache, config_result& out, std::string& error);

// %LOCALAPPDATA%/foo_librespot/cache, where the bridge keeps the audio cache
// (UTF-8). Empty if it cannot be determined.
std::string default_cache_dir();

// Worker thread. Signs out of Spotify: the bridge's `logout` op when a bridge
// answers (it deletes credentials.json and drops the session), otherwise the
// stored credentials.json in %APPDATA%\foo_librespot is deleted directly.
// Clears the %spotify_liked% set. Returns false with `error` on failure.
bool log_out_spotify(std::string& error);

// Size in bytes of the cached audio below `cache_dir` (0 if there is none).
long long cache_folder_bytes(const std::string& cache_dir);

// Deletes the cached audio below `cache_dir` directly, for when no bridge is
// running to do it. Returns false (with `error`) if something stayed behind.
bool clear_cache_folder(const std::string& cache_dir, std::string& error);

// The user-facing form of a bridge/pipe error ("Spotify: connecting failed —
// <reason>" and so on), for popups that show an error next to their context.
std::string user_error_message(const std::exception& error);

// GUID of the Spotify input (input_spotify.cpp), for album_art_extractor_v2.
const GUID& input_spotify_guid();

// Parses "spotify:track:<id>" paths.
bool is_spotify_track_path(const char* path);
bool is_spotify_track_uri(const std::string& uri);

// Canonical playlist path for a resolved track.
std::string track_path(const track_info& track);

} // namespace flp
