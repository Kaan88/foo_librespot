#pragma once

#include <SDK/cfg_var.h>
#include <SDK/advconfig_impl.h>

#include <string>

namespace flp {

// Bridge streaming bitrate in kbps; one of 96, 160, 320 (default 320).
extern cfg_uint cfg_bitrate_kbps;

// Audio cache size in MiB; 0 turns the cache off (default 1024 = 1 GB).
extern cfg_uint cfg_cache_mb;

// Automatic sync interval in minutes; 0 disables the timer (default 30).
extern cfg_uint cfg_sync_interval_minutes;

// Prefix for generated foobar2000 playlist names (default "Spotify · ").
extern cfg_string cfg_playlist_prefix;

// Report plays to Spotify (listening history, recently played); default on.
extern cfg_bool cfg_report_plays;

// Advanced > Tools > Spotify > Debug logging: the bridge writes debug
// lines to bridge.log (passed as --debug, and switched live with `config`).
extern advconfig_checkbox_factory cfg_debug_log;

constexpr int kDefaultBitrateKbps = 320;
constexpr int kDefaultSyncIntervalMinutes = 30;
constexpr int kDefaultCacheMb = 1024;
constexpr int kMaxCacheMb = 100 * 1024;
constexpr char kDefaultPlaylistPrefix[] = "Spotify \xC2\xB7 ";

bool is_valid_bitrate(int kbps);
int effective_bitrate_kbps();

// The configured cache size clamped to 0..kMaxCacheMb MiB.
int effective_cache_mb();

// The configured interval clamped to 0..7 days.
int effective_sync_interval_minutes();

// The configured prefix, never empty.
std::string effective_playlist_prefix();

} // namespace flp
