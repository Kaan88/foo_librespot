// Right-click "Spotify" submenu for selected spotify:track: items.
//
// Structure (SDK services):
//   - contextmenu_group_popup_factory      the "Spotify" submenu in the root menu
//   - contextmenu_item_simple              Save/Remove Liked Songs, Open album,
//                                          Open artist, Start radio, Copy link
//   - contextmenu_item_v2 (own node tree)  "Add to Spotify playlist" > playlists
//     contextmenu_item_simple cannot generate sub-items, so this one item
//     builds its popup node dynamically from the cached library.
//
// instantiate_item / context_get_display run on the main thread while the menu
// is being built and must not block: everything that needs the bridge (the
// saved state of a track, the editable-playlist list) is cached and refreshed
// in the background; actions run on worker threads (fb2k::splitTask) and
// report through popup_message (errors) and the console (success).

#include "stdafx.h"

#include "component_helpers.h"
#include "spotify_links.h"
#include "spotify_playlists.h"

#include "bridge/process.h"

#include <SDK/console.h>
#include <SDK/contextmenu.h>
#include <SDK/popup_message.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using clock_type = std::chrono::steady_clock;

const GUID guid_spotify_group = { 0x6b0e8d13, 0x2f47, 0x4a95, { 0xb1, 0x7c, 0x3e, 0x58, 0xd2, 0x09, 0xa4, 0x6f } };
const GUID guid_spotify_items = { 0xc4a79e52, 0x81d0, 0x4b36, { 0x95, 0x2e, 0x0a, 0x7f, 0x13, 0xb8, 0xd6, 0x41 } };
const GUID guid_spotify_add_to_playlist = { 0x0f58b3a7, 0xd641, 0x47ce, { 0x8d, 0x09, 0x62, 0xe1, 0xa3, 0x7c, 0x55, 0xb2 } };

constexpr size_t kMaxOpened = 5;  // albums / artists opened from one selection
constexpr size_t kInfoBatch = 25; // tracks asked about for album/artist lookups
constexpr auto kSavedTtl = std::chrono::minutes(5);
constexpr auto kPlaylistsTtl = std::chrono::minutes(2);

// --- helpers ------------------------------------------------------------------

std::vector<std::string> selected_track_uris(metadb_handle_list_cref data) {
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (t_size i = 0; i < data.get_count(); ++i) {
		const char* path = data[i]->get_path();
		if (!flp::is_spotify_track_path(path)) continue;
		if (seen.insert(path).second) out.emplace_back(path);
	}
	return out;
}

void show_message(const std::string& text, bool error) {
	fb2k::inMainThread([text, error] {
		popup_message::g_show(text.c_str(), "foo_librespot",
			error ? popup_message::icon_error : popup_message::icon_information);
	});
}

void log_line(const std::string& text) {
	console::print(("foo_librespot: " + text).c_str());
}

// Runs `task` on a worker thread with a started bridge; failures become a popup.
void run_bridge_task(std::function<void(flp::bridge_client&)> task) {
	fb2k::splitTask([task] {
		try {
			flp::ensure_bridge_started(fb2k::mainAborter());
			flp::bridge_client client;
			task(client);
		} catch (const flp::cancelled&) {
		} catch (const std::exception& error) {
			const std::string message = flp::user_error_message(error);
			log_line(message);
			show_message(message, true);
		}
	});
}

std::string plural(size_t count, const char* one, const char* many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

// --- removing tracks from the Spotify playlist ---------------------------------

struct removal_target {
	std::string uri;
	int index = 0;
};

// The active playlist, when it is a synced Spotify playlist (not an album,
// Liked Songs, radio or a detached playlist).
bool active_spotify_playlist(t_size& playlist, std::string& context) {
	playlist_manager::ptr manager = playlist_manager::get();
	const t_size active = manager->get_active_playlist();
	if (active == pfc::infinite_size) return false;
	std::string stored;
	if (!flp::playlist_context(active, stored)) return false;
	if (stored.rfind("spotify:playlist:", 0) != 0) return false;
	if (flp::is_static_playlist(active)) return false;
	playlist = active;
	context = stored;
	return true;
}

// Builds the request from the active playlist's selection: `data` must be
// exactly that selection, so every position is one the user actually selected.
bool build_removal(metadb_handle_list_cref data, t_size playlist,
	std::vector<removal_target>& out) {
	if (data.get_count() == 0) return false;

	std::map<std::string, int> wanted;
	for (t_size i = 0; i < data.get_count(); ++i) {
		const char* path = data[i]->get_path();
		if (path == nullptr || !flp::is_spotify_track_path(path)) return false;
		++wanted[path];
	}

	playlist_manager::ptr manager = playlist_manager::get();
	const t_size count = manager->playlist_get_item_count(playlist);
	bit_array_bittable selection(count);
	manager->playlist_get_selection_mask(playlist, selection);

	std::vector<std::pair<std::string, t_size>> positions;
	for (t_size i = 0; i < count; ++i) {
		if (!selection.get(i)) continue;
		metadb_handle_ptr handle;
		if (!manager->playlist_get_item_handle(handle, playlist, i)) return false;
		const char* path = handle->get_path();
		if (path == nullptr || !flp::is_spotify_track_path(path)) return false;
		positions.emplace_back(path, i);
	}
	if (positions.size() != data.get_count()) return false;

	out.clear();
	std::set<t_size> used;
	for (const auto& position : positions) {
		const auto found = wanted.find(position.first);
		if (found == wanted.end() || found->second <= 0) return false;
		--found->second;
		if (!used.insert(position.second).second) return false;
		out.push_back(removal_target{ position.first, static_cast<int>(position.second) });
	}
	for (const auto& entry : wanted) {
		if (entry.second != 0) return false;
	}
	return !out.empty();
}

std::string playlist_label(t_size playlist) {
	pfc::string8 name;
	if (playlist_manager::get()->playlist_get_name(playlist, name)) return name.get_ptr();
	return "the playlist";
}

// Main thread. Removes the already-removed-on-Spotify tracks from the local
// copy and stores the returned revision so the next sync does not re-add them.
void apply_removal(const std::string& context, const std::vector<flp::playlist_item_ref>& refs,
	const std::string& revision) {
	playlist_manager::ptr manager = playlist_manager::get();
	const t_size playlist = flp::find_playlist_by_context(context);
	if (playlist == pfc::infinite_size) {
		show_message("The Spotify playlist is no longer linked to a foobar2000 playlist.", true);
		return;
	}

	const t_size count = manager->playlist_get_item_count(playlist);
	bit_array_bittable mask(count);
	for (const flp::playlist_item_ref& ref : refs) {
		const t_size index = static_cast<t_size>(ref.index);
		metadb_handle_ptr handle;
		if (index >= count || !manager->playlist_get_item_handle(handle, playlist, index)) {
			show_message("The playlist changed while the tracks were being removed.\n"
				"Sync it and try again.", true);
			return;
		}
		const char* path = handle->get_path();
		if (path == nullptr || ref.uri != path) {
			show_message("The playlist changed while the tracks were being removed.\n"
				"Sync it and try again.", true);
			return;
		}
		mask.set(index, true);
	}
	if (!manager->playlist_remove_items(playlist, mask)) {
		show_message("The playlist is locked.", true);
		return;
	}
	flp::set_playlist_revision(playlist, revision);
	log_line("removed " + plural(refs.size(), "track", "tracks") + " from \"" +
		playlist_label(playlist) + "\" on Spotify");
}

void action_remove_from_playlist(metadb_handle_list_cref data) {
	t_size playlist = pfc::infinite_size;
	std::string context;
	if (!active_spotify_playlist(playlist, context)) return;

	std::vector<removal_target> targets;
	if (!build_removal(data, playlist, targets)) {
		show_message("Select the tracks in the synced Spotify playlist to remove.", true);
		return;
	}
	const std::string name = playlist_label(playlist);
	if (targets.size() > 1) {
		const std::string message = "Remove " + std::to_string(targets.size()) + " tracks from \"" +
			name + "\" on Spotify?\n\nThey are removed from this foobar2000 playlist too.";
		popup_message_v3::query_t query;
		query.title = "Remove from Spotify playlist";
		query.msg = message.c_str();
		query.buttons = popup_message_v3::buttonYes | popup_message_v3::buttonNo;
		query.defButton = popup_message_v3::buttonNo;
		query.icon = popup_message_v3::iconQuestion;
		query.wndParent = core_api::get_main_window();
		if (popup_message_v3::get()->show_query_modal(query) != popup_message_v3::buttonYes) return;
	}

	std::vector<flp::playlist_item_ref> refs;
	refs.reserve(targets.size());
	for (const removal_target& target : targets) {
		refs.push_back(flp::playlist_item_ref{ target.uri, target.index });
	}

	fb2k::splitTask([context, refs] {
		std::string revision;
		std::string error;
		try {
			flp::ensure_bridge_started(fb2k::mainAborter());
			flp::bridge_client client;
			revision = client.playlist_remove(context, refs, flp::cancel_token());
		} catch (const flp::cancelled&) {
			return;
		} catch (const std::exception& failure) {
			error = flp::user_error_message(failure);
		}
		fb2k::inMainThread([context, refs, revision, error] {
			if (!error.empty()) {
				log_line(error);
				show_message(error, true);
				return;
			}
			apply_removal(context, refs, revision);
		});
	});
}

// --- caches ---------------------------------------------------------------------

struct editable_playlist {
	std::string uri;
	std::string name;
};

class menu_cache {
public:
	static menu_cache& get() {
		static menu_cache cache;
		return cache;
	}

	// Saved state of a track: true when known and fresh.
	bool saved_state(const std::string& uri, bool& saved) {
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto found = m_saved.find(uri);
		if (found == m_saved.end() || clock_type::now() - found->second.when > kSavedTtl) {
			return false;
		}
		saved = found->second.saved;
		return true;
	}

	void set_saved(const std::vector<std::string>& uris, bool saved) {
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const std::string& uri : uris) m_saved[uri] = saved_entry{ saved, clock_type::now() };
	}

	void refresh_saved_async(const std::string& uri) {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_savedInFlight.count(uri) != 0) return;
			m_savedInFlight.insert(uri);
		}
		fb2k::splitTask([this, uri] {
			try {
				flp::ensure_bridge_started(fb2k::mainAborter());
				flp::bridge_client client;
				const std::vector<bool> flags = client.saved({ uri }, flp::cancel_token());
				if (!flags.empty()) set_saved({ uri }, flags[0]);
			} catch (...) {
				// The menu then keeps offering both entries; nothing to report.
			}
			std::lock_guard<std::mutex> lock(m_mutex);
			m_savedInFlight.erase(uri);
		});
	}

	// Editable playlists from the last library call.
	std::vector<editable_playlist> playlists(bool& loaded) {
		std::lock_guard<std::mutex> lock(m_mutex);
		loaded = m_playlistsLoaded;
		return m_playlists;
	}

	void refresh_playlists_if_stale() {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_playlistsInFlight) return;
			if (m_playlistsLoaded && clock_type::now() - m_playlistsWhen < kPlaylistsTtl) return;
			m_playlistsInFlight = true;
		}
		fb2k::splitTask([this] {
			std::vector<editable_playlist> found;
			bool ok = false;
			try {
				flp::ensure_bridge_started(fb2k::mainAborter());
				flp::bridge_client client;
				for (const flp::library_section& section : client.library(flp::cancel_token())) {
					for (const flp::library_item& item : section.items) {
						if (item.kind == "playlist" && item.editable) {
							found.push_back(editable_playlist{ item.uri, item.name });
						}
					}
				}
				ok = true;
			} catch (...) {
			}
			std::lock_guard<std::mutex> lock(m_mutex);
			if (ok) {
				m_playlists = std::move(found);
				m_playlistsLoaded = true;
				m_playlistsWhen = clock_type::now();
			}
			m_playlistsInFlight = false;
		});
	}

private:
	struct saved_entry {
		bool saved = false;
		clock_type::time_point when;
	};
	std::mutex m_mutex;
	std::map<std::string, saved_entry> m_saved;
	std::set<std::string> m_savedInFlight;
	std::vector<editable_playlist> m_playlists;
	bool m_playlistsLoaded = false;
	bool m_playlistsInFlight = false;
	clock_type::time_point m_playlistsWhen;
};

// --- actions --------------------------------------------------------------------

void action_save(const std::vector<std::string>& uris, bool save) {
	run_bridge_task([uris, save](flp::bridge_client& client) {
		if (save) client.save(uris, flp::cancel_token());
		else client.unsave(uris, flp::cancel_token());
		menu_cache::get().set_saved(uris, save);
		log_line(std::string(save ? "saved " : "removed ") + plural(uris.size(), "track", "tracks") +
			(save ? " to Liked Songs" : " from Liked Songs"));
		// Liked Songs playlists pick the change up now.
		fb2k::inMainThread([] { flp::sync_all_tracked_async(); });
	});
}

void report_open(bool ok, const std::string& message) {
	if (!ok && !message.empty()) {
		log_line(message);
		show_message(message, true);
	}
}

void action_open_album(const std::vector<std::string>& uris) {
	run_bridge_task([uris](flp::bridge_client& client) {
		const std::vector<std::string> first(uris.begin(),
			uris.begin() + static_cast<std::ptrdiff_t>(std::min(uris.size(), kInfoBatch)));
		const std::vector<flp::track_info> infos = client.info(first, flp::cancel_token());
		std::vector<std::pair<std::string, std::string>> albums; // uri, name
		std::set<std::string> seen;
		for (const flp::track_info& track : infos) {
			if (track.album_uri.empty() || !seen.insert(track.album_uri).second) continue;
			albums.emplace_back(track.album_uri, track.album);
		}
		if (albums.empty()) {
			show_message("The bridge did not report an album for the selected track(s).\n"
				"Open album needs a librespot-bridge that sends album_uri (protocol v3).", true);
			return;
		}
		if (albums.size() > kMaxOpened) albums.resize(kMaxOpened);
		fb2k::inMainThread([albums] {
			for (const auto& album : albums) {
				flp::open_context_async(album.first, album.second, false, false,
					[](bool ok, const std::string& message, t_size) { report_open(ok, message); });
			}
		});
	});
}

void action_open_artist(const std::vector<std::string>& uris) {
	run_bridge_task([uris](flp::bridge_client& client) {
		const std::vector<std::string> first(uris.begin(),
			uris.begin() + static_cast<std::ptrdiff_t>(std::min(uris.size(), kInfoBatch)));
		const std::vector<flp::track_info> infos = client.info(first, flp::cancel_token());
		std::vector<std::pair<std::string, std::string>> artists; // uri, name
		std::set<std::string> seen;
		for (const flp::track_info& track : infos) {
			if (track.artist_uris.empty() || !seen.insert(track.artist_uris.front()).second) continue;
			artists.emplace_back(track.artist_uris.front(),
				track.artists.empty() ? std::string() : track.artists.front());
		}
		if (artists.empty()) {
			show_message("The bridge did not report an artist for the selected track(s).\n"
				"Open artist needs a librespot-bridge that sends artist_uris (protocol v3).", true);
			return;
		}
		if (artists.size() > kMaxOpened) artists.resize(kMaxOpened);
		fb2k::inMainThread([artists] {
			for (const auto& artist : artists) {
				const std::string uri = artist.first;
				const std::string fallbackName = artist.second.empty() ? "Artist" : artist.second;
				flp::open_snapshot_async(uri, fallbackName + " \xE2\x80\x93 top tracks",
					[uri, fallbackName](flp::bridge_client& bridge, const flp::cancel_token& ct) {
						const flp::artist_result page = bridge.artist(uri, ct);
						flp::resolve_result out;
						out.tracks = page.top_tracks;
						const std::string name = page.artist.name.empty() ? fallbackName : page.artist.name;
						out.context.name = name + " \xE2\x80\x93 top tracks";
						return out;
					},
					[](bool ok, const std::string& message, t_size) { report_open(ok, message); });
			}
		});
	});
}

void action_radio(const std::vector<std::string>& uris) {
	if (uris.empty()) return;
	const std::string seed = uris.front();
	if (uris.size() > 1) log_line("radio is based on the first selected track");
	flp::open_snapshot_async("radio:" + seed, "Radio",
		[seed](flp::bridge_client& client, const flp::cancel_token& ct) {
			return client.radio(seed, ct);
		},
		[](bool ok, const std::string& message, t_size) { report_open(ok, message); });
}

void action_add_to_playlist(const std::vector<std::string>& uris, const std::string& playlist,
	const std::string& name) {
	run_bridge_task([uris, playlist, name](flp::bridge_client& client) {
		client.playlist_add(playlist, uris, flp::cancel_token());
		log_line("added " + plural(uris.size(), "track", "tracks") + " to \"" + name + "\"");
		// A synced foobar2000 copy of that playlist picks the new tracks up now.
		fb2k::inMainThread([] { flp::sync_all_tracked_async(); });
	});
}

void copy_text_to_clipboard(const std::string& text) {
	const std::wstring wide = flp::wide_from_utf8(text);
	if (!OpenClipboard(core_api::get_main_window())) {
		show_message("Could not open the clipboard.", true);
		return;
	}
	EmptyClipboard();
	const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (memory != nullptr) {
		void* ptr = GlobalLock(memory);
		if (ptr != nullptr) {
			memcpy(ptr, wide.c_str(), bytes);
			GlobalUnlock(memory);
			if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) GlobalFree(memory);
		} else {
			GlobalFree(memory);
		}
	}
	CloseClipboard();
}

void action_copy_links(const std::vector<std::string>& uris) {
	std::string text;
	size_t count = 0;
	for (const std::string& uri : uris) {
		const std::string url = flp::spotify_web_url(uri);
		if (url.empty()) continue;
		text += url;
		text += "\r\n";
		++count;
	}
	if (count == 0) return;
	copy_text_to_clipboard(text);
	log_line("copied " + plural(count, "Spotify link", "Spotify links"));
}

// --- menu: simple items ------------------------------------------------------------

enum item_index : unsigned {
	item_save,
	item_unsave,
	item_album,
	item_artist,
	item_radio,
	item_copy_link,
	item_remove_playlist,
	item_count,
};

class spotify_menu_items : public contextmenu_item_simple {
public:
	unsigned get_num_items() override { return item_count; }

	void get_item_name(unsigned index, pfc::string_base& out) override {
		switch (index) {
		case item_save: out = "Save to Liked Songs"; break;
		case item_unsave: out = "Remove from Liked Songs"; break;
		case item_album: out = "Open album"; break;
		case item_artist: out = "Open artist"; break;
		case item_radio: out = "Start radio"; break;
		case item_copy_link: out = "Copy Spotify link"; break;
		case item_remove_playlist: out = "Remove from Spotify playlist"; break;
		default: break;
		}
	}

	GUID get_item_guid(unsigned index) override {
		GUID guid = guid_spotify_items;
		guid.Data1 += index;
		return guid;
	}

	bool get_item_description(unsigned index, pfc::string_base& out) override {
		switch (index) {
		case item_save: out = "Adds the selected Spotify tracks to your Liked Songs"; return true;
		case item_unsave: out = "Removes the selected Spotify tracks from your Liked Songs"; return true;
		case item_album: out = "Opens the album of the selected track as a synced playlist"; return true;
		case item_artist: out = "Opens the top tracks of the track's artist as a playlist"; return true;
		case item_radio: out = "Opens a Spotify radio based on the selected track"; return true;
		case item_copy_link: out = "Copies open.spotify.com links to the clipboard"; return true;
		case item_remove_playlist:
			out = "Removes the selected tracks from this Spotify playlist and from foobar2000";
			return true;
		default: return false;
		}
	}

	GUID get_parent() override { return guid_spotify_group; }
	double get_sort_priority() override { return 0; }

	bool context_get_display(unsigned index, metadb_handle_list_cref data, pfc::string_base& out,
		unsigned& flags, const GUID&) override {
		flags = 0;
		const std::vector<std::string> uris = selected_track_uris(data);
		if (uris.empty()) return false;
		get_item_name(index, out);

		if (index == item_remove_playlist) {
			t_size playlist = pfc::infinite_size;
			std::string context;
			std::vector<removal_target> targets;
			if (!active_spotify_playlist(playlist, context)) return false;
			return build_removal(data, playlist, targets);
		}

		if (index == item_save || index == item_unsave) {
			if (uris.size() == 1) {
				bool saved = false;
				if (menu_cache::get().saved_state(uris.front(), saved)) {
					// Only the entry that makes sense for this track.
					return index == (saved ? item_unsave : item_save);
				}
				menu_cache::get().refresh_saved_async(uris.front()); // right next time
			}
			// Several tracks, or state not known yet: offer both.
		}
		return true;
	}

	void context_command(unsigned index, metadb_handle_list_cref data, const GUID&) override {
		const std::vector<std::string> uris = selected_track_uris(data);
		if (uris.empty()) return;
		switch (index) {
		case item_save: action_save(uris, true); break;
		case item_unsave: action_save(uris, false); break;
		case item_album: action_open_album(uris); break;
		case item_artist: action_open_artist(uris); break;
		case item_radio: action_radio(uris); break;
		case item_copy_link: action_copy_links(uris); break;
		case item_remove_playlist: action_remove_from_playlist(data); break;
		default: break;
		}
	}
};

// --- menu: Add to Spotify playlist (dynamic submenu) -----------------------------------

class playlist_leaf : public contextmenu_item_node_leaf {
public:
	playlist_leaf(std::string label, unsigned flags, std::function<void()> action)
		: m_label(std::move(label)), m_flags(flags), m_action(std::move(action)) {}

	bool get_display_data(pfc::string_base& out, unsigned& flags, metadb_handle_list_cref,
		const GUID&) override {
		out = m_label.c_str();
		flags = m_flags;
		return true;
	}
	void execute(metadb_handle_list_cref, const GUID&) override {
		if (m_action) m_action();
	}
	bool get_description(pfc::string_base&) override { return false; }
	GUID get_guid() override { return pfc::guid_null; }
	bool is_mappable_shortcut() override { return false; }

private:
	std::string m_label;
	unsigned m_flags;
	std::function<void()> m_action;
};

class add_to_playlist_root : public contextmenu_item_node_root_popup {
public:
	explicit add_to_playlist_root(const std::vector<std::string>& uris) {
		bool loaded = false;
		const std::vector<editable_playlist> playlists = menu_cache::get().playlists(loaded);
		menu_cache::get().refresh_playlists_if_stale();
		if (!loaded) {
			m_children.push_back(std::make_unique<playlist_leaf>("Loading playlists\xE2\x80\xA6",
				static_cast<unsigned>(contextmenu_item_simple::FLAG_DISABLED_GRAYED),
				std::function<void()>()));
		} else if (playlists.empty()) {
			m_children.push_back(std::make_unique<playlist_leaf>("No editable playlists",
				static_cast<unsigned>(contextmenu_item_simple::FLAG_DISABLED_GRAYED),
				std::function<void()>()));
		}
		for (const editable_playlist& playlist : playlists) {
			const std::string uri = playlist.uri;
			const std::string name = playlist.name.empty() ? uri : playlist.name;
			m_children.push_back(std::make_unique<playlist_leaf>(name, 0u,
				[uris, uri, name] { action_add_to_playlist(uris, uri, name); }));
		}
	}

	bool get_display_data(pfc::string_base& out, unsigned& flags, metadb_handle_list_cref,
		const GUID&) override {
		out = "Add to Spotify playlist";
		flags = 0;
		return true;
	}
	t_size get_children_count() override { return m_children.size(); }
	contextmenu_item_node* get_child(t_size index) override {
		return index < m_children.size() ? m_children[index].get() : nullptr;
	}
	GUID get_guid() override { return guid_spotify_add_to_playlist; }
	bool is_mappable_shortcut() override { return false; }

private:
	std::vector<std::unique_ptr<playlist_leaf>> m_children;
};

class spotify_add_to_playlist_item : public contextmenu_item_v2 {
public:
	unsigned get_num_items() override { return 1; }

	contextmenu_item_node_root* instantiate_item(unsigned, metadb_handle_list_cref data,
		const GUID&) override {
		const std::vector<std::string> uris = selected_track_uris(data);
		if (uris.empty()) return nullptr;
		return new add_to_playlist_root(uris);
	}

	GUID get_item_guid(unsigned) override { return guid_spotify_add_to_playlist; }
	void get_item_name(unsigned, pfc::string_base& out) override { out = "Add to Spotify playlist"; }
	bool get_item_description(unsigned, pfc::string_base& out) override {
		out = "Appends the selected Spotify tracks to one of your Spotify playlists";
		return true;
	}
	t_enabled_state get_enabled_state(unsigned) override { return contextmenu_item::DEFAULT_ON; }
	void item_execute_simple(unsigned, const GUID&, metadb_handle_list_cref, const GUID&) override {}
	GUID get_parent() override { return guid_spotify_group; }
	double get_sort_priority() override { return 1; }
};

static contextmenu_group_popup_factory g_spotify_group(guid_spotify_group, contextmenu_groups::root,
	"Spotify", static_cast<double>(contextmenu_priorities::root_utilities) + 0.5);
static contextmenu_item_factory_t<spotify_menu_items> g_spotify_items_factory;
static contextmenu_item_factory_t<spotify_add_to_playlist_item> g_spotify_add_factory;

} // namespace
