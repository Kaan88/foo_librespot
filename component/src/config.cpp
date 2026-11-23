#include "stdafx.h"
#include "config.h"

namespace flp {
namespace {
// Identifies our setting inside the component configuration store.
const GUID guid_cfg_bitrate_kbps = { 0x6b0b1a4e, 0x9c31, 0x4c1e, { 0xa7, 0x2d, 0x51, 0x2f, 0x0b, 0xe3, 0x9a, 0x71 } };
const GUID guid_cfg_sync_interval = { 0x2f7c4a91, 0x8b6d, 0x4e02, { 0xa3, 0x5f, 0x7c, 0x18, 0x44, 0x9d, 0xb2, 0x60 } };
const GUID guid_cfg_playlist_prefix = { 0x9d4e7b13, 0x3a52, 0x4c8f, { 0xb1, 0x67, 0x0e, 0x9a, 0xd5, 0x24, 0x7c, 0x38 } };
} // namespace

cfg_uint cfg_bitrate_kbps(guid_cfg_bitrate_kbps, kDefaultBitrateKbps);
cfg_uint cfg_sync_interval_minutes(guid_cfg_sync_interval, kDefaultSyncIntervalMinutes);
cfg_string cfg_playlist_prefix(guid_cfg_playlist_prefix, kDefaultPlaylistPrefix);

bool is_valid_bitrate(int kbps) { return kbps == 96 || kbps == 160 || kbps == 320; }

int effective_bitrate_kbps() {
	const int value = static_cast<int>(cfg_bitrate_kbps.get());
	return is_valid_bitrate(value) ? value : kDefaultBitrateKbps;
}

int effective_sync_interval_minutes() {
	const long long value = cfg_sync_interval_minutes.get();
	if (value == 0) return 0;
	if (value < 0) return kDefaultSyncIntervalMinutes;
	if (value > 7 * 24 * 60) return 7 * 24 * 60;
	return static_cast<int>(value);
}

std::string effective_playlist_prefix() {
	pfc::string8 value;
	cfg_playlist_prefix.get(value);
	if (value.is_empty()) return kDefaultPlaylistPrefix;
	return std::string(value.c_str());
}

} // namespace flp
