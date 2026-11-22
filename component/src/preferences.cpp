#include "stdafx.h"

#include "component_helpers.h"
#include "config.h"
#include "resource.h"

#include "bridge/process.h"

#include <SDK/popup_message.h>

#include <string>

namespace {

const GUID guid_preferences_page = { 0x1d0f4a6c, 0x77b1, 0x4a5e, { 0x8e, 0x0e, 0x6c, 0x59, 0x2a, 0xd3, 0x7e, 0x41 } };

// Plain Win32 preferences page; no ATL/WTL dependency.
class preferences_instance : public preferences_page_instance {
public:
	preferences_instance(HWND parent, preferences_page_callback::ptr callback)
		: m_callback(callback) {
		flp::init_bridge_paths();
		m_wnd = CreateDialogParamW(core_api::get_my_instance(),
			MAKEINTRESOURCEW(IDD_PREFERENCES), parent, &preferences_instance::dialog_proc,
			reinterpret_cast<LPARAM>(this));
		if (m_wnd == nullptr) {
			throw exception_win32(GetLastError());
		}
	}

	~preferences_instance() {
		if (m_wnd != nullptr && IsWindow(m_wnd)) DestroyWindow(m_wnd);
	}

	t_uint32 get_state() override {
		t_uint32 state = 0;
		if (selected_bitrate() != flp::effective_bitrate_kbps()) state |= preferences_state::changed;
		return state;
	}

	HWND get_wnd() override { return m_wnd; }

	void apply() override {
		flp::cfg_bitrate_kbps.set(selected_bitrate());
		refresh_status();
		m_callback->on_state_changed();
	}

	void reset() override {
		set_bitrate_selection(flp::kDefaultBitrateKbps);
		m_callback->on_state_changed();
	}

private:
	static INT_PTR CALLBACK dialog_proc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
		if (message == WM_INITDIALOG) {
			auto* self = reinterpret_cast<preferences_instance*>(lparam);
			SetWindowLongPtrW(wnd, GWLP_USERDATA, lparam);
			self->m_wnd = wnd;
			self->on_init();
			return TRUE;
		}
		auto* self = reinterpret_cast<preferences_instance*>(
			GetWindowLongPtrW(wnd, GWLP_USERDATA));
		if (self != nullptr) return self->on_message(wnd, message, wparam, lparam);
		return FALSE;
	}

	void on_init() {
		SendDlgItemMessageW(m_wnd, IDC_BITRATE, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(L"96 kbps"));
		SendDlgItemMessageW(m_wnd, IDC_BITRATE, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(L"160 kbps"));
		SendDlgItemMessageW(m_wnd, IDC_BITRATE, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(L"320 kbps (default)"));
		set_bitrate_selection(flp::effective_bitrate_kbps());
		refresh_status();
	}

	INT_PTR on_message(HWND wnd, UINT message, WPARAM wparam, LPARAM) {
		switch (message) {
		case WM_COMMAND:
			switch (LOWORD(wparam)) {
			case IDC_LOGIN:
				if (HIWORD(wparam) == BN_CLICKED) on_login();
				return TRUE;
			case IDC_REFRESH:
				if (HIWORD(wparam) == BN_CLICKED) refresh_status();
				return TRUE;
			case IDC_BITRATE:
				if (HIWORD(wparam) == CBN_SELCHANGE) m_callback->on_state_changed();
				return TRUE;
			default:
				break;
			}
			break;
		case WM_NCDESTROY:
			SetWindowLongPtrW(wnd, GWLP_USERDATA, 0);
			m_wnd = nullptr;
			break;
		default:
			break;
		}
		(void)wparam;
		return FALSE;
	}

	void on_login() {
		std::string error;
		if (flp::bridge_process::instance().launch_login(error)) {
			set_status_text("A login window was opened. Complete the Spotify login there, "
							"then click Refresh.");
		} else {
			set_status_text("Could not start the login helper: " + error);
			popup_message::g_show(error.c_str(), "foo_librespot", popup_message::icon_error);
		}
	}

	void set_status_text(const std::string& text) {
		if (m_wnd == nullptr) return;
		SetDlgItemTextW(m_wnd, IDC_STATUS, flp::wide_from_utf8(text).c_str());
	}

	void refresh_status() {
		flp::bridge_status status;
		std::string error;
		if (!flp::bridge_process::instance().probe(status, error, 800)) {
			set_status_text("Bridge not running. It starts automatically when a Spotify track is "
							"played; use Log in... to sign in first.");
			return;
		}
		if (!status.logged_in) {
			const std::string version = status.version.empty() ? std::string() : " v" + status.version;
			set_status_text("Bridge running" + version + " but not logged in. Click Log in... to "
							"sign in.");
			return;
		}
		std::string text = "Bridge running, logged in as " + status.username;
		if (!status.country.empty()) text += " (" + status.country + ")";
		if (status.state == "error") {
			text += ". Spotify is unavailable";
			text += status.last_error.empty() ? "." : ": " + status.last_error + ".";
		} else if (status.state == "connected") {
			text += ". Spotify connected";
		} else {
			text += ". Connecting to Spotify...";
		}
		if (!status.version.empty()) text += " Bridge v" + status.version + ".";
		set_status_text(text);
	}

	void set_bitrate_selection(int kbps) {
		int index = 2;
		if (kbps == 96) index = 0;
		else if (kbps == 160) index = 1;
		SendDlgItemMessageW(m_wnd, IDC_BITRATE, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
	}

	int selected_bitrate() const {
		const LRESULT index = SendDlgItemMessageW(m_wnd, IDC_BITRATE, CB_GETCURSEL, 0, 0);
		switch (index) {
		case 0: return 96;
		case 1: return 160;
		default: return 320;
		}
	}

	preferences_page_callback::ptr m_callback;
	HWND m_wnd = nullptr;
};

class preferences_page_spotify : public preferences_page_v3 {
public:
	const char* get_name() override { return "foo_librespot"; }
	GUID get_guid() override { return guid_preferences_page; }
	GUID get_parent_guid() override { return preferences_page::guid_tools; }

	preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent,
		preferences_page_callback::ptr callback) override {
		return fb2k::service_new<preferences_instance>(static_cast<HWND>(parent), callback);
	}
};

static preferences_page_factory_t<preferences_page_spotify> g_preferences_page_factory;

} // namespace
