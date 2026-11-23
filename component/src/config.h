#pragma once

#include <SDK/cfg_var.h>

#include <string>

namespace flp {

// Bridge streaming bitrate in kbps; one of 96, 160, 320 (default 320).
extern cfg_uint cfg_bitrate_kbps;

// Automatic sync interval in minutes; 0 disables the timer (default 30).
extern cfg_uint cfg_sync_interval_minutes;

// Prefix for generated foobar2000 playlist names (default "Spotify · ").
extern cfg_string cfg_playlist_prefix;

constexpr int kDefaultBitrateKbps = 320;
constexpr int kDefaultSyncIntervalMinutes = 30;
constexpr char kDefaultPlaylistPrefix[] = "Spotify \xC2\xB7 ";

bool is_valid_bitrate(int kbps);
int effective_bitrate_kbps();

// The configured interval clamped to 0..7 days.
int effective_sync_interval_minutes();

// The configured prefix, never empty.
std::string effective_playlist_prefix();

} // namespace flp
