#pragma once

#include <SDK/cfg_var.h>

namespace flp {

// Bridge streaming bitrate in kbps; one of 96, 160, 320 (default 320).
extern cfg_uint cfg_bitrate_kbps;

constexpr int kDefaultBitrateKbps = 320;

bool is_valid_bitrate(int kbps);
int effective_bitrate_kbps();

} // namespace flp
