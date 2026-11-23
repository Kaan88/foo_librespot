#pragma once

// Master SDK header (includes service framework, input, playlist, metadb,
// preferences, menu, file_info, album art, HTTP client, ...).
//
// NOMINMAX keeps std::min/std::max usable; the SDK's windows.h include comes
// first, so the definition has to be here rather than in each source file.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SDK/foobar2000.h>
