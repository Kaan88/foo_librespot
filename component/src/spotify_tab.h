// "Spotify" panel: a search box and a library tree.
//
// The panel is a plain Win32 child window (no ATL/WTL), which keeps the
// component self-contained. Two hosts wrap the same panel code:
//   - a Default UI element (spotify_tab.cpp), added by Default UI's
//     "Add New UI Element..." dialog, grouped under Media Library viewers;
//   - a Columns UI panel (spotify_tab_cui.cpp, uie::window), listed under
//     Panels when Columns UI is installed.
// The host supplies colours, fonts and the dark mode flag.

#pragma once

#include <SDK/ui_element.h>

#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace flp {

extern const GUID spotify_tab_guid;

// What the panel asks of its host. Main thread only. Fonts stay owned by the
// host and must outlive the panel (or the next on_style_changed()).
class spotify_panel_host {
public:
	virtual ~spotify_panel_host() = default;
	virtual bool panel_dark_mode() = 0;
	// false: use the panel's own default for the colour.
	virtual bool panel_background(COLORREF& out) = 0;
	virtual bool panel_text(COLORREF& out) = 0;
	// nullptr: keep the control's current font.
	virtual HFONT panel_list_font() = 0;
	virtual HFONT panel_default_font() = 0;
};

class spotify_panel {
public:
	virtual ~spotify_panel() = default;
	virtual HWND get_wnd() const = 0;
	// Colours, fonts or dark mode changed.
	virtual void on_style_changed() = 0;
	virtual void set_default_focus() = 0;
};

// Creates the panel window as a child of `parent`. `visible` adds WS_VISIBLE
// (Default UI); Columns UI shows the window itself. Throws on failure.
std::unique_ptr<spotify_panel> create_spotify_panel(HWND parent, spotify_panel_host& host,
	bool visible);

} // namespace flp
