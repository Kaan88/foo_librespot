// "Spotify" Default UI element: a search box and a library tree.
//
// The element is a plain Win32 child window (no ATL/WTL), which keeps the
// component self-contained. It is added as a tab by Default UI's
// "Add New UI Element..." dialog, grouped under Media Library viewers.

#pragma once

#include <SDK/ui_element.h>

namespace flp {

extern const GUID spotify_tab_guid;

} // namespace flp
