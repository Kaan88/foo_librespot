#include "stdafx.h"

#include "config.h"
#include "spotify_playlists.h"

#include <SDK/contextmenu.h>
#include <SDK/initquit.h>
#include <SDK/menu.h>
#include <SDK/popup_message.h>

#include <atomic>
#include <chrono>
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

class detach_command : public mainmenu_commands {
public:
	t_uint32 get_command_count() override { return 1; }
	GUID get_command(t_uint32) override { return guid_detach_command; }
	void get_name(t_uint32, pfc::string_base& out) override { out = "Detach from Spotify"; }
	bool get_description(t_uint32, pfc::string_base& out) override {
		out = "Removes the Spotify sync link from the active playlist; the playlist itself is kept";
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

// Right-click on a playlist (tab or Playlist Manager): remove the sync link.
class detach_context_node : public contextmenu_item_node_root_leaf {
public:
	bool get_display_data(pfc::string_base& out, unsigned& flags, metadb_handle_list_cref,
		const GUID&) override {
		out = "Detach from Spotify";
		flags = 0;
		return true;
	}
	void execute(metadb_handle_list_cref, const GUID&) override {
		flp::detach_active_playlist();
	}
	bool get_description(pfc::string_base& out) override {
		out = "Removes the Spotify sync link from this playlist; the playlist itself is kept";
		return true;
	}
	GUID get_guid() override { return guid_detach_context_item; }
	bool is_mappable_shortcut() override { return false; }
};

class detach_context_item : public contextmenu_item_v2 {
public:
	unsigned get_num_items() override { return 1; }

	contextmenu_item_node_root* instantiate_item(unsigned, metadb_handle_list_cref,
		const GUID& caller) override {
		if (caller != contextmenu_item::caller_active_playlist &&
			caller != contextmenu_item::caller_playlist_manager &&
			caller != contextmenu_item::caller_playlist) {
			return nullptr;
		}
		playlist_manager::ptr manager = playlist_manager::get();
		const t_size active = manager->get_active_playlist();
		if (active == pfc::infinite_size) return nullptr;
		std::string context;
		if (!flp::playlist_context(active, context) || context.empty()) return nullptr;
		return new detach_context_node();
	}

	GUID get_item_guid(unsigned) override { return guid_detach_context_item; }
	void get_item_name(unsigned, pfc::string_base& out) override { out = "Detach from Spotify"; }
	bool get_item_description(unsigned, pfc::string_base& out) override {
		out = "Removes the Spotify sync link from this playlist; the playlist itself is kept";
		return true;
	}
	t_enabled_state get_enabled_state(unsigned) override { return contextmenu_item::DEFAULT_ON; }
	void item_execute_simple(unsigned, const GUID&, metadb_handle_list_cref, const GUID&) override {
		flp::detach_active_playlist();
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
				fb2k::inMainThread([] { flp::sync_all_tracked_async(); });
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
