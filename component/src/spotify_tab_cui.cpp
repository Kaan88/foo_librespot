// Columns UI host for the "Spotify" panel (spotify_tab.h).
//
// Listed in Columns UI's layout editor under Panels > Spotify. The tree and
// search code is shared with the Default UI element; this file only supplies
// colours (Columns UI global colours), fonts (Columns UI common items and
// labels fonts) and the dark mode flag, and forwards their change
// notifications. Without Columns UI installed the service is simply unused.

#include "stdafx.h"

#include "spotify_tab.h"

#include <columns_ui-sdk/ui_extension.h>

#include <memory>

namespace flp {
namespace {

// {6f3a1d84-92c5-4b7e-a0d6-3e5b17c9f428}
constexpr GUID kSpotifyCuiPanelGuid = { 0x6f3a1d84, 0x92c5, 0x4b7e,
	{ 0xa0, 0xd6, 0x3e, 0x5b, 0x17, 0xc9, 0xf4, 0x28 } };

class spotify_cui_panel : public uie::window,
						  private spotify_panel_host,
						  private cui::colours::common_callback,
						  private cui::fonts::common_callback {
public:
	~spotify_cui_panel() { teardown(); }

	const GUID& get_extension_guid() const override { return kSpotifyCuiPanelGuid; }
	void get_name(pfc::string_base& out) const override { out = "Spotify"; }
	void get_category(pfc::string_base& out) const override { out = "Panels"; }
	bool get_description(pfc::string_base& out) const override {
		out = "Browse your Spotify library and open playlists, albums, artists and Liked Songs "
			  "as synced foobar2000 playlists.";
		return true;
	}
	unsigned get_type() const override { return uie::type_panel; }
	bool is_available(const uie::window_host_ptr&) const override { return true; }

	HWND create_or_transfer_window(HWND parent, const uie::window_host_ptr& host,
		const ui_helpers::window_position_t& position) override {
		if (m_panel) {
			// Transfer to another host: keep the window and its state.
			ShowWindow(m_panel->get_wnd(), SW_HIDE);
			SetParent(m_panel->get_wnd(), parent);
			// The old host must stop treating the window as its own.
			if (m_host.is_valid()) m_host->relinquish_ownership(m_panel->get_wnd());
			m_host = host;
		} else {
			m_host = host;
			load_fonts();
			try {
				m_panel = create_spotify_panel(parent, *this, false);
			} catch (const std::exception&) {
				teardown();
				return nullptr;
			}
			register_callbacks();
		}
		SetWindowPos(m_panel->get_wnd(), nullptr, position.x, position.y, position.cx,
			position.cy, SWP_NOZORDER);
		return m_panel->get_wnd();
	}

	void destroy_window() override { teardown(); }

	HWND get_wnd() const override { return m_panel ? m_panel->get_wnd() : nullptr; }

private:
	void teardown() {
		unregister_callbacks();
		m_panel.reset(); // destroys the window before the fonts it uses
		free_fonts();
		m_host.release();
	}

	// ---- spotify_panel_host ----
	bool panel_dark_mode() override { return cui::colours::helper().is_dark_mode_active(); }
	bool panel_background(COLORREF& out) override {
		out = cui::colours::helper().get_colour(cui::colours::colour_background);
		return true;
	}
	bool panel_text(COLORREF& out) override {
		out = cui::colours::helper().get_colour(cui::colours::colour_text);
		return true;
	}
	HFONT panel_list_font() override { return m_items_font; }
	HFONT panel_default_font() override { return m_labels_font; }

	// ---- Columns UI notifications ----
	void on_colour_changed(uint32_t) const override { restyle(false); }
	void on_bool_changed(uint32_t mask) const override {
		if ((mask & cui::colours::bool_flag_dark_mode_enabled) != 0) restyle(false);
	}
	void on_font_changed(uint32_t) const override { restyle(true); }

	// The callbacks are const in the SDK; restyling changes our fonts.
	void restyle(bool fonts) const {
		auto* self = const_cast<spotify_cui_panel*>(this);
		if (!self->m_panel) return;
		HFONT oldItems = nullptr;
		HFONT oldLabels = nullptr;
		if (fonts) {
			oldItems = self->m_items_font;
			oldLabels = self->m_labels_font;
			self->m_items_font = nullptr;
			self->m_labels_font = nullptr;
			self->load_fonts();
		}
		self->m_panel->on_style_changed();
		// Only now are the controls off the old fonts.
		if (oldItems != nullptr) DeleteObject(oldItems);
		if (oldLabels != nullptr) DeleteObject(oldLabels);
	}

	void load_fonts() {
		cui::fonts::manager::ptr fonts;
		if (!fb2k::std_api_try_get(fonts)) return;
		m_items_font = fonts->get_font(cui::fonts::font_type_items);
		m_labels_font = fonts->get_font(cui::fonts::font_type_labels);
	}

	void free_fonts() {
		if (m_items_font != nullptr) DeleteObject(m_items_font);
		if (m_labels_font != nullptr) DeleteObject(m_labels_font);
		m_items_font = nullptr;
		m_labels_font = nullptr;
	}

	void register_callbacks() {
		if (m_registered) return;
		if (fb2k::std_api_try_get(m_colours)) {
			m_colours->register_common_callback(
				static_cast<cui::colours::common_callback*>(this));
		}
		if (fb2k::std_api_try_get(m_fonts)) {
			m_fonts->register_common_callback(static_cast<cui::fonts::common_callback*>(this));
		}
		m_registered = true;
	}

	void unregister_callbacks() {
		if (!m_registered) return;
		if (m_colours.is_valid()) {
			m_colours->deregister_common_callback(
				static_cast<cui::colours::common_callback*>(this));
		}
		if (m_fonts.is_valid()) {
			m_fonts->deregister_common_callback(static_cast<cui::fonts::common_callback*>(this));
		}
		m_colours.release();
		m_fonts.release();
		m_registered = false;
	}

	uie::window_host_ptr m_host;
	cui::colours::manager::ptr m_colours;
	cui::fonts::manager::ptr m_fonts;
	bool m_registered = false;
	HFONT m_items_font = nullptr;
	HFONT m_labels_font = nullptr;
	std::unique_ptr<spotify_panel> m_panel;
};

uie::window_factory<spotify_cui_panel> g_spotify_cui_panel;

} // namespace
} // namespace flp
