#include "stdafx.h"

#include "component_helpers.h"
#include "resource.h"

#include "bridge/process.h"

#include <SDK/popup_message.h>
#include <SDK/core_api.h>
#include <SDK/menu.h>
#include <SDK/metadb.h>
#include <SDK/playlist.h>

#include <set>
#include <string>
#include <vector>

namespace {

const GUID guid_add_link_command = { 0x8b3b6f0a, 0x51d2, 0x4e77, { 0xa0, 0x4c, 0x6c, 0x1f, 0x2d, 0xc4, 0x9b, 0x33 } };

constexpr char kNotLoggedInMessage[] =
	"Spotify: not logged in \xE2\x80\x94 Preferences > Tools > foo_librespot";

// Plain Win32 dialog: a multiline edit plus OK/Cancel.
class add_link_dialog {
public:
	std::string text;

	INT_PTR run(HWND parent) {
		return DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_ADD_LINK),
			parent, &add_link_dialog::dialog_proc, reinterpret_cast<LPARAM>(this));
	}

private:
	static INT_PTR CALLBACK dialog_proc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
		if (message == WM_INITDIALOG) {
			auto* self = reinterpret_cast<add_link_dialog*>(lparam);
			SetWindowLongPtrW(wnd, GWLP_USERDATA, lparam);
			SetDlgItemTextW(wnd, IDC_HINT,
				L"Paste Spotify URIs (spotify:track:..., spotify:album:..., spotify:playlist:...)\n"
				L"or open.spotify.com links, one per line:");
			SetDlgItemTextW(wnd, IDC_LINKS, L"");
			SetFocus(GetDlgItem(wnd, IDC_LINKS));
			return FALSE;
		}

		auto* self = reinterpret_cast<add_link_dialog*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
		if (self == nullptr) return FALSE;

		if (message == WM_COMMAND) {
			switch (LOWORD(wparam)) {
			case IDOK: {
				HWND edit = GetDlgItem(wnd, IDC_LINKS);
				const int length = GetWindowTextLengthW(edit);
				if (length <= 0) {
					EndDialog(wnd, IDCANCEL);
					return TRUE;
				}
				std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
				GetWindowTextW(edit, wide.data(), length + 1);
				wide.resize(static_cast<size_t>(length));
				self->text = flp::utf8_from_wide(wide);
				EndDialog(wnd, IDOK);
				return TRUE;
			}
			case IDCANCEL:
				EndDialog(wnd, IDCANCEL);
				return TRUE;
			default:
				break;
			}
		}
		return FALSE;
	}
};

std::string trim(const std::string& value) {
	size_t begin = 0;
	size_t end = value.size();
	while (begin < end && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r')) {
		++begin;
	}
	while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
		--end;
	}
	std::string out = value.substr(begin, end - begin);
	if (out.size() >= 2) {
		const char first = out.front();
		const char last = out.back();
		if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
			out = out.substr(1, out.size() - 2);
		}
	}
	return out;
}

std::vector<std::string> split_lines(const std::string& text) {
	std::vector<std::string> lines;
	std::string current;
	for (const char c : text) {
		if (c == '\n') {
			const std::string line = trim(current);
			if (!line.empty()) lines.push_back(line);
			current.clear();
		} else {
			current.push_back(c);
		}
	}
	const std::string line = trim(current);
	if (!line.empty()) lines.push_back(line);
	return lines;
}

void insert_into_active_playlist(const std::vector<flp::track_info>& tracks) {
	metadb_handle_list handles;
	metadb_hint_list_v3::ptr hints = metadb_hint_list_v3::create();
	std::set<std::string> seen;

	for (const flp::track_info& track : tracks) {
		const std::string path = flp::track_path(track);
		if (path.empty() || !seen.insert(path).second) continue;

		metadb_handle_ptr handle = metadb::get()->handle_create(path.c_str(), 0);
		handles.add_item(handle);

		file_info_impl info;
		flp::fill_file_info(track, info);
		hints->add_hint_forced(handle, info, t_filestats(), false);
	}

	if (handles.get_count() == 0) return;
	hints->on_done();

	playlist_manager::ptr manager = playlist_manager::get();
	t_size playlist = manager->get_active_playlist();
	if (playlist == pfc::infinite_size) {
		playlist = manager->create_playlist_autoname();
		manager->set_active_playlist(playlist);
	}
	const t_size base = manager->playlist_get_item_count(playlist);
	manager->playlist_insert_items(playlist, base, handles, bit_array_true());
}

class add_link_command : public mainmenu_commands {
public:
	t_uint32 get_command_count() override { return 1; }

	GUID get_command(t_uint32) override { return guid_add_link_command; }

	void get_name(t_uint32, pfc::string_base& out) override { out = "Add Spotify link..."; }

	bool get_description(t_uint32, pfc::string_base& out) override {
		out = "Add Spotify tracks, albums or playlists to the active playlist";
		return true;
	}

	GUID get_parent() override { return mainmenu_groups::file; }

	t_uint32 get_sort_priority() override { return mainmenu_commands::sort_priority_base; }

	void execute(t_uint32, service_ptr_t<service_base>) override {
		const HWND parent = core_api::get_main_window();

		add_link_dialog dialog;
		if (dialog.run(parent) != IDOK) return;

		const std::vector<std::string> inputs = split_lines(dialog.text);
		if (inputs.empty()) return;

		try {
			flp::ensure_bridge_started(fb2k::noAbort);
		} catch (const std::exception& error) {
			popup_message::g_show(error.what(), "foo_librespot", popup_message::icon_error);
			return;
		}

		std::vector<flp::track_info> resolved;
		std::string failures;

		for (const std::string& input : inputs) {
			try {
				flp::bridge_client client;
				const std::vector<flp::track_info> tracks =
					client.resolve(input, flp::make_cancel(fb2k::noAbort));
				resolved.insert(resolved.end(), tracks.begin(), tracks.end());
			} catch (const flp::cancelled&) {
				return;
			} catch (const flp::bridge_error& error) {
				if (error.code == "not_logged_in") {
					popup_message::g_show(kNotLoggedInMessage, "foo_librespot",
						popup_message::icon_error);
					return;
				}
				failures += input + ": " + error.what() + "\n";
				console::print("foo_librespot: resolve failed for ", input.c_str(), ": ", error.what());
			} catch (const std::exception& error) {
				failures += input + ": " + error.what() + "\n";
				console::print("foo_librespot: resolve failed for ", input.c_str(), ": ", error.what());
			}
		}

		if (resolved.empty()) {
			const std::string message = failures.empty()
				? "No tracks found in the supplied link(s)."
				: ("Could not resolve the supplied link(s):\n" + failures);
			popup_message::g_show(message.c_str(), "foo_librespot", popup_message::icon_error);
			return;
		}

		try {
			insert_into_active_playlist(resolved);
			console::print("foo_librespot: added ", pfc::format_int(resolved.size()).get_ptr(),
				" track(s) to the active playlist");
			if (!failures.empty()) {
				popup_message::g_show(("Some links could not be resolved:\n" + failures).c_str(),
					"foo_librespot", popup_message::icon_information);
			}
		} catch (const std::exception& error) {
			popup_message::g_show(error.what(), "foo_librespot", popup_message::icon_error);
		}
	}
};

static service_factory_single_t<add_link_command> g_add_link_command_factory;

} // namespace
