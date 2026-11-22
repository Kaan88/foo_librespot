#pragma once

// Maps bridge track metadata onto foobar2000's file_info. Kept free of
// component/host APIs so it can be unit-tested by tools/metadata_probe.

#include "bridge/protocol.h"

#include <SDK/foobar2000-lite.h>

namespace flp {

void fill_file_info(const track_info& track, file_info& info);

} // namespace flp
