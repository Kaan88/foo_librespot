#include "stdafx.h"

#include "component_helpers.h"
#include "config.h"
#include "resource.h"

#include "bridge/process.h"

#include <SDK/popup_message.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace {

const GUID guid_preferences_page = { 0x1d0f4a6c, 0x77b1, 0x4a5e, { 0x8e, 0x0e, 0x6c, 0x59, 0x2a, 0xd3, 0x7e, 0x41 } };

// Lets the status worker post its result without touching a destroyed page.
struct status_state {
	std::atomic<HWND> target{ nullptr };
	std::atomic<uint64_t> generation{ 0 };
};

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
		m_status->target = m_wnd;
	}

	~preferences_instance() {
		if (m_wnd != nullptr && IsWindow(m_wnd)) DestroyWindow(m_wnd);
	}

	t_uint32 get_state() override {
		t_uint32 state = 0;
		if (selected_bitrate() != flp::effective_bitrate_kbps()) state |= preferences_state::changed;
		if (selected_interval() != flp::effective_sync_interval_minutes()) {
			state |= preferences_state::changed;
		}
		if (effective_prefix_text() != flp::effective_playlist_prefix()) {
			state |= preferences_state::changed;
		}
		return state;
	}

	HWND get_wnd() override { return m_wnd; }

	void apply() override {
		flp::cfg_bitrate_kbps.set(selected_bitrate());
		flp::cfg_sync_interval_minutes.set(selected_interval());
		std::string prefix = effective_prefix_text();
		flp::cfg_playlist_prefix.set(prefix.c_str());
		SetDlgItemTextW(m_wnd, IDC_PREFIX, flp::wide_from_utf8(prefix).c_str());
		refresh_status();
		m_callback->on_state_changed();
	}

	void reset() override {
		set_bitrate_selection(flp::kDefaultBitrateKbps);
		set_interval_selection(flp::kDefaultSyncIntervalMinutes);
		SetDlgItemTextW(m_wnd, IDC_PREFIX, flp::wide_from_utf8(flp::kDefaultPlaylistPrefix).c_str());
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
		m_status->target = m_wnd;
		SendDlgItemMessageW(m_wnd, IDC_BITRATE, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(L"96 kbps"));
		SendDlgItemMessageW(m_wnd, IDC_BITRATE, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(L"160 kbps"));
		SendDlgItemMessageW(m_wnd, IDC_BITRATE, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(L"320 kbps (default)"));
		set_bitrate_selection(flp::effective_bitrate_kbps());

		build_sync_choices();
		SetDlgItemTextW(m_wnd, IDC_PREFIX,
			flp::wide_from_utf8(flp::effective_playlist_prefix()).c_str());

		refresh_status();
	}

	void build_sync_choices() {
		m_syncValues = { 0, 15, 30, 60, 120 };
		const int current = flp::effective_sync_interval_minutes();
		if (current > 0 && std::find(m_syncValues.begin(), m_syncValues.end(), current) ==
				m_syncValues.end()) {
			m_syncValues.push_back(current);
			std::sort(m_syncValues.begin(), m_syncValues.end());
		}

		for (const int minutes : m_syncValues) {
			std::wstring label;
			if (minutes == 0) {
				label = L"Off";
			} else if (minutes == 15) {
				label = L"Every 15 minutes";
			} else if (minutes == 30) {
				label = L"Every 30 minutes (default)";
			} else if (minutes == 60) {
				label = L"Every hour";
			} else if (minutes == 120) {
				label = L"Every 2 hours";
			} else {
				label = L"Every " + std::to_wstring(minutes) + L" minutes";
			}
			SendDlgItemMessageW(m_wnd, IDC_SYNC, CB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(label.c_str()));
		}
		set_interval_selection(current);
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
			case IDC_SYNC:
				if (HIWORD(wparam) == CBN_SELCHANGE) m_callback->on_state_changed();
				return TRUE;
			case IDC_PREFIX:
				if (HIWORD(wparam) == EN_CHANGE) m_callback->on_state_changed();
				return TRUE;
			default:
				break;
			}
			break;
		case WM_NCDESTROY:
			SetWindowLongPtrW(wnd, GWLP_USERDATA, 0);
			++m_status->generation;
			m_status->target = nullptr;
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
			++m_status->generation; // keep a pending status probe from overwriting this
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
		if (m_wnd == nullptr) return;
		set_status_text("Checking the bridge...");

		const uint64_t generation = ++m_status->generation;
		const std::shared_ptr<status_state> state = m_status;
		fb2k::splitTask([state, generation] {
			flp::bridge_status status;
			std::string error;
			std::string text;
			if (!flp::bridge_process::instance().probe(status, error, 800)) {
				text = "Bridge not running. It starts automatically when a Spotify track is "
					   "played; use Log in... to sign in first.";
			} else if (!status.logged_in) {
				const std::string version =
					status.version.empty() ? std::string() : " v" + status.version;
				text = "Bridge running" + version + " but not logged in. Click Log in... to "
					   "sign in.";
			} else {
				text = "Bridge running, logged in as " + status.username;
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
			}

			fb2k::inMainThread([state, generation, text] {
				if (state->generation.load() != generation) return;
				HWND target = state->target.load();
				if (target == nullptr || !IsWindow(target)) return;
				SetDlgItemTextW(target, IDC_STATUS, flp::wide_from_utf8(text).c_str());
			});
		});
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

	void set_interval_selection(int minutes) {
		LRESULT index = 0;
		for (size_t i = 0; i < m_syncValues.size(); ++i) {
			if (m_syncValues[i] == minutes) {
				index = static_cast<LRESULT>(i);
				break;
			}
		}
		SendDlgItemMessageW(m_wnd, IDC_SYNC, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
	}

	int selected_interval() const {
		const LRESULT index = SendDlgItemMessageW(m_wnd, IDC_SYNC, CB_GETCURSEL, 0, 0);
		if (index < 0 || static_cast<size_t>(index) >= m_syncValues.size()) {
			return flp::kDefaultSyncIntervalMinutes;
		}
		return m_syncValues[static_cast<size_t>(index)];
	}

	std::string prefix_text() const {
		const int length = GetWindowTextLengthW(GetDlgItem(m_wnd, IDC_PREFIX));
		if (length <= 0) return std::string();
		std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
		GetWindowTextW(GetDlgItem(m_wnd, IDC_PREFIX), wide.data(), length + 1);
		wide.resize(static_cast<size_t>(length));
		return flp::utf8_from_wide(wide);
	}

	std::string effective_prefix_text() const {
		const std::string prefix = prefix_text();
		return prefix.empty() ? std::string(flp::kDefaultPlaylistPrefix) : prefix;
	}

	preferences_page_callback::ptr m_callback;
	HWND m_wnd = nullptr;
	std::vector<int> m_syncValues;
	std::shared_ptr<status_state> m_status = std::make_shared<status_state>();
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
