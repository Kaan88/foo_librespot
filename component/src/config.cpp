#include "stdafx.h"
#include "config.h"

namespace flp {
namespace {
// Identifies our setting inside the component configuration store.
const GUID guid_cfg_bitrate_kbps = { 0x6b0b1a4e, 0x9c31, 0x4c1e, { 0xa7, 0x2d, 0x51, 0x2f, 0x0b, 0xe3, 0x9a, 0x71 } };
} // namespace

cfg_uint cfg_bitrate_kbps(guid_cfg_bitrate_kbps, kDefaultBitrateKbps);

bool is_valid_bitrate(int kbps) { return kbps == 96 || kbps == 160 || kbps == 320; }

int effective_bitrate_kbps() {
	const int value = static_cast<int>(cfg_bitrate_kbps.get());
	return is_valid_bitrate(value) ? value : kDefaultBitrateKbps;
}

} // namespace flp
