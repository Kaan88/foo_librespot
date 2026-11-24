#include "stdafx.h"

#include "config.h"
#include "spotify_playlists.h"

#include <SDK/console.h>
#include <SDK/contextmenu.h>
#include <SDK/initquit.h>
#include <SDK/menu.h>
#include <SDK/popup_message.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

const GUID guid_sync_all_command = { 0x5a41c7d2, 0x3b68, 0x4e90, { 0x9c, 0x14, 0x7e, 0xb5, 0x28, 0x63, 0x0d, 0xf7 } };
const GUID guid_detach_command = { 0x2d80f4b6, 0x9e51, 0x4a37, { 0x83, 0xcf, 0x10, 0x5b, 0x6a, 0x92, 0x47, 0xe1 } };
const GUID guid_detach_context_item = { 0xf1b64c09, 0x7d23, 0x4e58, { 0xa2, 0x8b, 0x34, 0xc9, 0x05, 0x76, 0xde, 0x18 } };

class sync_all_command : public mainmenu_commands {
public:
	t_uint32 get_command_count() override { return 1; }
	GUID get_command(t_uint32) override { return guid_sync_all_command; }
	void get_name(t_uint32, pfc::string_base& out) override { out = "Sync Spotify playlists"; }
	bool get_description(t_uint32, pfc::string_base& out) override {
		out = "Refreshes every playlist that is linked to a Spotify playlist, album or Liked Songs";
		return true;
	}
	GUID get_parent() override { return mainmenu_groups::library; }
	t_uint32 get_sort_priority() override {
		return mainmenu_commands::sort_priority_base + 20;
	}
	void execute(t_uint32, service_ptr_t<service_base>) override {
		flp::sync_all_tracked_async();
	}
};

bool is_linked(t_size playlist) {
	if (playlist == pfc::infinite_size) return false;
	std::string context;
	return flp::playlist_context(playlist, context) && !context.empty();
}

bool active_is_linked() { return is_linked(playlist_manager::get()->get_active_playlist()); }

class detach_command : public mainmenu_commands {
public:
	t_uint32 get_command_count() override { return 1; }
	GUID get_command(t_uint32) override { return guid_detach_command; }
	void get_name(t_uint32, pfc::string_base& out) override { out = "Detach from Spotify"; }
	bool get_description(t_uint32, pfc::string_base& out) override {
		out = "Removes the Spotify sync link from the active playlist; the playlist itself is kept";
		return true;
	}
	// Greyed out unless the active playlist is linked to Spotify.
	bool get_display(t_uint32 index, pfc::string_base& text, t_uint32& flags) override {
		get_name(index, text);
		flags = active_is_linked() ? 0 : mainmenu_commands::flag_disabled;
		return true;
	}
	GUID get_parent() override { return mainmenu_groups::edit; }
	t_uint32 get_sort_priority() override {
		return mainmenu_commands::sort_priority_base + 30;
	}
	void execute(t_uint32, service_ptr_t<service_base>) override {
		if (!flp::detach_active_playlist()) {
			popup_message::g_show("The active playlist is not linked to Spotify.", "foo_librespot",
				popup_message::icon_information);
		}
	}
};

bool same_items(t_size playlist, metadb_handle_list_cref data) {
	playlist_manager::ptr manager = playlist_manager::get();
	if (manager->playlist_get_item_count(playlist) != data.get_count()) return false;
	metadb_handle_list items;
	manager->playlist_get_all_items(playlist, items);
	if (items.get_count() != data.get_count()) return false;
	for (t_size i = 0; i < items.get_count(); ++i) {
		if (items[i] != data[i]) return false;
	}
	return true;
}

// The playlist a playlist-level context menu is about. Playlist views and the
// active-playlist callers mean the active playlist. The Playlist Manager and
// playlist tabs pass the right-clicked playlist's items without its index, so
// that playlist is the linked one holding exactly those items (the active one
// wins a tie, e.g. between two empty playlists). Returns the context URI, or
// empty when there is no single linked match.
std::string target_context(metadb_handle_list_cref data, const GUID& caller) {
	playlist_manager::ptr manager = playlist_manager::get();
	const t_size active = manager->get_active_playlist();
	std::string context;
	if (caller != contextmenu_item::caller_playlist_manager) {
		if (is_linked(active)) flp::playlist_context(active, context);
		return context;
	}
	if (is_linked(active) && same_items(active, data)) {
		flp::playlist_context(active, context);
		return context;
	}
	std::string match;
	size_t matches = 0;
	const t_size count = manager->get_playlist_count();
	for (t_size i = 0; i < count; ++i) {
		std::string candidate;
		if (!flp::playlist_context(i, candidate) || candidate.empty()) continue;
		if (!same_items(i, data)) continue;
		match = candidate;
		++matches;
	}
	return matches == 1 ? match : std::string();
}

void detach_by_context(const std::string& context) {
	const t_size playlist = flp::find_playlist_by_context(context);
	if (playlist == pfc::infinite_size) return;
	if (flp::detach_playlist(playlist)) {
		pfc::string8 name;
		playlist_manager::get()->playlist_get_name(playlist, name);
		console::print("foo_librespot: detached \"", name.get_ptr(), "\" from Spotify");
	}
}

// Right-click on a playlist (tab or Playlist Manager): remove the sync link.
// Only offered for linked playlists; it acts on the playlist it was opened for.
class detach_context_node : public contextmenu_item_node_root_leaf {
public:
	explicit detach_context_node(std::string context) : m_context(std::move(context)) {}

	bool get_display_data(pfc::string_base& out, unsigned& flags, metadb_handle_list_cref,
		const GUID&) override {
		out = "Detach from Spotify";
		flags = 0;
		return true;
	}
	void execute(metadb_handle_list_cref, const GUID&) override { detach_by_context(m_context); }
	bool get_description(pfc::string_base& out) override {
		out = "Removes the Spotify sync link from this playlist; the playlist itself is kept";
		return true;
	}
	GUID get_guid() override { return guid_detach_context_item; }
	bool is_mappable_shortcut() override { return false; }

private:
	std::string m_context;
};

class detach_context_item : public contextmenu_item_v2 {
public:
	unsigned get_num_items() override { return 1; }

	contextmenu_item_node_root* instantiate_item(unsigned, metadb_handle_list_cref data,
		const GUID& caller) override {
		if (caller != contextmenu_item::caller_active_playlist &&
			caller != contextmenu_item::caller_playlist_manager &&
			caller != contextmenu_item::caller_playlist) {
			return nullptr;
		}
		std::string context = target_context(data, caller);
		if (context.empty()) return nullptr;
		return new detach_context_node(std::move(context));
	}

	GUID get_item_guid(unsigned) override { return guid_detach_context_item; }
	void get_item_name(unsigned, pfc::string_base& out) override { out = "Detach from Spotify"; }
	bool get_item_description(unsigned, pfc::string_base& out) override {
		out = "Removes the Spotify sync link from this playlist; the playlist itself is kept";
		return true;
	}
	t_enabled_state get_enabled_state(unsigned) override { return contextmenu_item::DEFAULT_ON; }
	void item_execute_simple(unsigned, const GUID&, metadb_handle_list_cref data,
		const GUID& caller) override {
		const std::string context = target_context(data, caller);
		if (!context.empty()) detach_by_context(context);
	}
	GUID get_parent() override { return contextmenu_groups::utilities; }
};

// Background timer for "sync every N minutes" (0 disables it).
class sync_timer : public initquit {
public:
	void on_init() override {
		m_stop = true;
		if (m_thread.joinable()) m_thread.join();
		m_stop = false;
		m_thread = std::thread([this] { run(); });
	}

	void on_quit() override {
		m_stop = true;
		if (m_thread.joinable()) m_thread.join();
	}

private:
	void run() {
		using clock = std::chrono::steady_clock;
		clock::time_point last = clock::now();
		int lastInterval = 0;

		while (!m_stop) {
			for (int slice = 0; slice < 10 && !m_stop; ++slice) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
			if (m_stop) break;

			const int interval = flp::effective_sync_interval_minutes();
			const clock::time_point now = clock::now();
			if (interval <= 0) {
				last = now;
				lastInterval = 0;
				continue;
			}
			if (interval != lastInterval) {
				// A changed interval starts a fresh period.
				last = now;
				lastInterval = interval;
				continue;
			}
			if (now - last >= std::chrono::minutes(interval)) {
				last = now;
				fb2k::inMainThread([] { flp::sync_all_tracked_async(true); });
			}
		}
	}

	std::atomic<bool> m_stop{ false };
	std::thread m_thread;
};

static service_factory_single_t<sync_all_command> g_sync_all_command_factory;
static service_factory_single_t<detach_command> g_detach_command_factory;
static contextmenu_item_factory_t<detach_context_item> g_detach_context_factory;
static initquit_factory_t<sync_timer> g_sync_timer_factory;

} // namespace
