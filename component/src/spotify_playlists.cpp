#include "stdafx.h"

#include "spotify_playlists.h"

#include "component_helpers.h"
#include "config.h"
#include "sync_plan.h"

#include "bridge/process.h"

#include <SDK/console.h>
#include <SDK/metadb.h>

#include <algorithm>
#include <atomic>
#include <map>

namespace flp {
namespace {

// "foo_librespot.context": the Spotify context URI of a synced playlist.
const GUID kPlaylistContextGuid = { 0x7c2f5a41, 0x0d63, 0x4b9e, { 0x8a, 0x31, 0x5f, 0xc4, 0x72, 0x09, 0xbe, 0x56 } };
// "foo_librespot.revision": the context revision last applied.
const GUID kPlaylistRevisionGuid = { 0x1e94b7d0, 0x6a58, 0x4f21, { 0xb3, 0x47, 0xd8, 0x0c, 0x95, 0x62, 0x1f, 0xa8 } };

std::string property_to_string(const pfc::array_t<t_uint8>& data) {
	if (data.get_size() == 0) return std::string();
	return std::string(reinterpret_cast<const char*>(data.get_ptr()), data.get_size());
}

bool get_property(t_size playlist, const GUID& property, std::string& out) {
	playlist_manager_v2::ptr manager = playlist_manager_v2::get();
	pfc::array_t<t_uint8> data;
	if (!manager->playlist_get_property(playlist, property, data)) return false;
	out = property_to_string(data);
	return true;
}

void set_property(t_size playlist, const GUID& property, const std::string& value) {
	playlist_manager_v2::ptr manager = playlist_manager_v2::get();
	pfc::array_t<t_uint8> data;
	data.set_size(value.size());
	if (!value.empty()) {
		memcpy(data.get_ptr(), value.data(), value.size());
	}
	manager->playlist_set_property(playlist, property, data);
}

std::string item_path(const metadb_handle_ptr& handle) {
	if (handle.is_empty()) return std::string();
	const char* path = handle->get_path();
	return path == nullptr ? std::string() : std::string(path);
}

// Creates a hint list for the tracks that are about to be inserted, so titles,
// album grouping and durations are available before the renderer asks for them.
void preload_tracks(const std::vector<sync_insertion>& insertions,
	const std::map<std::string, const track_info*>& by_uri) {
	if (insertions.empty()) return;

	metadb_hint_list_v3::ptr hints = metadb_hint_list_v3::create();
	for (const sync_insertion& insertion : insertions) {
		const auto found = by_uri.find(insertion.uri);
		if (found == by_uri.end()) continue;
		metadb_handle_ptr handle = metadb::get()->handle_create(insertion.uri.c_str(), 0);
		file_info_impl info;
		fill_file_info(*found->second, info);
		hints->add_hint_forced(handle, info, t_filestats(), false);
	}
	hints->on_done();
}

} // namespace

const GUID playlist_context_property = kPlaylistContextGuid;
const GUID playlist_revision_property = kPlaylistRevisionGuid;

std::string playlist_display_name(const std::string& name) {
	const std::string prefix = effective_playlist_prefix();
	const std::string display = name.empty() ? std::string("Spotify") : name;
	return prefix + display;
}

bool playlist_context(t_size playlist, std::string& out) {
	return get_property(playlist, kPlaylistContextGuid, out);
}

bool playlist_revision(t_size playlist, std::string& out) {
	return get_property(playlist, kPlaylistRevisionGuid, out);
}

bool set_playlist_context(t_size playlist, const std::string& context_uri) {
	if (context_uri.empty()) return false;
	set_property(playlist, kPlaylistContextGuid, context_uri);
	return true;
}

bool set_playlist_revision(t_size playlist, const std::string& revision) {
	set_property(playlist, kPlaylistRevisionGuid, revision);
	return true;
}

t_size find_playlist_by_context(const std::string& context_uri) {
	if (context_uri.empty()) return pfc::infinite_size;
	playlist_manager::ptr manager = playlist_manager::get();
	const t_size count = manager->get_playlist_count();
	for (t_size i = 0; i < count; ++i) {
		std::string stored;
		if (get_property(i, kPlaylistContextGuid, stored) && stored == context_uri) return i;
	}
	return pfc::infinite_size;
}

std::vector<tracked_playlist> list_tracked_playlists() {
	std::vector<tracked_playlist> out;
	playlist_manager::ptr manager = playlist_manager::get();
	const t_size count = manager->get_playlist_count();
	for (t_size i = 0; i < count; ++i) {
		std::string stored;
		if (!get_property(i, kPlaylistContextGuid, stored) || stored.empty()) continue;
		out.push_back(tracked_playlist{ i, stored });
	}
	return out;
}

apply_result apply_context(const std::string& context_uri, const std::string& name,
	const std::string& revision, const std::vector<track_info>& tracks, bool force) {
	apply_result result;
	if (context_uri.empty()) {
		result.error = "Spotify: empty context URI";
		return result;
	}

	playlist_manager::ptr manager = playlist_manager::get();
	t_size playlist = find_playlist_by_context(context_uri);
	if (playlist == pfc::infinite_size) {
		const std::string displayName = playlist_display_name(name);
		playlist = manager->create_playlist(displayName.c_str(), ~0, ~0);
		if (playlist == pfc::infinite_size) {
			result.error = "Spotify: could not create the playlist";
			return result;
		}
		set_playlist_context(playlist, context_uri);
		result.created = true;
	}

	result.valid = true;
	result.playlist = playlist;
	result.name = name;

	// An unchanged revision means Spotify reports the same contents; leave the
	// playlist alone (including any local edits the user made).
	std::string storedRevision;
	if (!force && !revision.empty() && get_property(playlist, kPlaylistRevisionGuid, storedRevision) &&
		storedRevision == revision) {
		return result;
	}

	const t_size oldCount = manager->playlist_get_item_count(playlist);
	std::vector<std::string> oldUris;
	oldUris.reserve(oldCount);
	for (t_size i = 0; i < oldCount; ++i) {
		metadb_handle_ptr handle;
		if (manager->playlist_get_item_handle(handle, playlist, i)) {
			oldUris.push_back(item_path(handle));
		} else {
			oldUris.push_back(std::string());
		}
	}

	std::vector<std::string> newUris;
	std::map<std::string, const track_info*> byUri;
	for (const track_info& track : tracks) {
		const std::string path = track_path(track);
		if (path.empty()) continue;
		newUris.push_back(path);
		byUri.emplace(path, &track);
	}

	// Keep the playing item and the selection when duplicates leave a choice.
	std::vector<size_t> protect;
	t_size playingPlaylist = pfc::infinite_size;
	t_size playingItem = pfc::infinite_size;
	if (manager->get_playing_item_location(&playingPlaylist, &playingItem) &&
		playingPlaylist == playlist && playingItem < oldCount) {
		protect.push_back(static_cast<size_t>(playingItem));
	}
	if (oldCount > 0) {
		bit_array_bittable selection(oldCount);
		manager->playlist_get_selection_mask(playlist, selection);
		for (t_size i = 0; i < oldCount; ++i) {
			if (selection.get(i)) protect.push_back(static_cast<size_t>(i));
		}
	}

	const sync_plan plan = make_sync_plan(oldUris, newUris, protect);
	result.added = plan.insert.size();
	result.removed = plan.remove.size();
	if (plan.no_change()) {
		set_playlist_revision(playlist, revision);
		return result;
	}

	preload_tracks(plan.insert, byUri);
	manager->playlist_undo_backup(playlist);

	if (!plan.remove.empty()) {
		bit_array_bittable mask(oldCount);
		for (const size_t index : plan.remove) {
			if (index < oldCount) mask.set(index, true);
		}
		if (!manager->playlist_remove_items(playlist, mask)) {
			result.error = "Spotify: the playlist is locked";
			return result;
		}
	}

	// Reorder the surviving items into Spotify's order. Reordering keeps the
	// playing item playing and the selection attached to its items.
	if (plan.survivor_order != plan.survivors && plan.survivors.size() > 1) {
		const t_size count = plan.survivors.size();
		pfc::array_t<t_size> order;
		order.set_size(count);
		for (t_size i = 0; i < count; ++i) {
			const auto position = std::lower_bound(plan.survivors.begin(), plan.survivors.end(),
				plan.survivor_order[i]);
			order[i] = static_cast<t_size>(position - plan.survivors.begin());
		}
		if (!manager->playlist_reorder_items(playlist, order.get_ptr(), count)) {
			result.error = "Spotify: the playlist is locked";
			return result;
		}
	}

	for (const sync_insertion& insertion : plan.insert) {
		metadb_handle_ptr handle = metadb::get()->handle_create(insertion.uri.c_str(), 0);
		pfc::list_single_ref_t<metadb_handle_ptr> one(handle);
		const t_size inserted = manager->playlist_insert_items(playlist, insertion.index, one,
			bit_array_false());
		if (inserted == pfc::infinite_size) {
			result.error = "Spotify: the playlist is locked";
			return result;
		}
	}

	set_playlist_revision(playlist, revision);
	result.changed = true;
	return result;
}

bool detach_playlist(t_size playlist) {
	playlist_manager_v2::ptr manager = playlist_manager_v2::get();
	if (playlist == pfc::infinite_size) return false;
	const bool detached = manager->playlist_remove_property(playlist, kPlaylistContextGuid);
	manager->playlist_remove_property(playlist, kPlaylistRevisionGuid);
	return detached;
}

bool detach_active_playlist() {
	playlist_manager::ptr manager = playlist_manager::get();
	const t_size active = manager->get_active_playlist();
	if (active == pfc::infinite_size) return false;
	if (!detach_playlist(active)) return false;
	console::print("foo_librespot: detached the active playlist from Spotify");
	return true;
}

void insert_tracks_into_active(const std::vector<track_info>& tracks) {
	if (tracks.empty()) return;

	metadb_handle_list handles;
	metadb_hint_list_v3::ptr hints = metadb_hint_list_v3::create();
	for (const track_info& track : tracks) {
		const std::string path = track_path(track);
		if (path.empty()) continue;
		metadb_handle_ptr handle = metadb::get()->handle_create(path.c_str(), 0);
		handles.add_item(handle);
		file_info_impl info;
		fill_file_info(track, info);
		hints->add_hint_forced(handle, info, t_filestats(), false);
	}
	if (handles.get_count() == 0) return;
	hints->on_done();

	playlist_manager::ptr manager = playlist_manager::get();
	t_size playlist = manager->get_active_playlist();
	if (playlist == pfc::infinite_size) {
		playlist = manager->create_playlist_autoname();
		if (playlist == pfc::infinite_size) return;
		manager->set_active_playlist(playlist);
	}
	const t_size base = manager->playlist_get_item_count(playlist);
	manager->playlist_insert_items(playlist, base, handles, bit_array_true());
}

void open_context_async(const std::string& context_uri, const std::string& name, bool play,
	bool force, operation_done done, cancel_token cancel) {
	fb2k::splitTask([context_uri, name, play, force, done, cancel] {
		resolve_result resolved;
		std::string error;
		try {
			ensure_bridge_started(fb2k::mainAborter());
			bridge_client client;
			resolved = client.resolve_context(context_uri, cancel);
			if (resolved.tracks.empty() && resolved.context.name.empty()) {
				error = "Spotify: no tracks found";
			}
		} catch (const cancelled&) {
			return;
		} catch (const std::exception& failure) {
			error = user_error_message(failure);
		}

		fb2k::inMainThread([context_uri, name, play, force, done, resolved, error] {
			if (!error.empty()) {
				if (done) done(false, error, pfc::infinite_size);
				return;
			}
			const std::string displayName =
				resolved.context.name.empty() ? name : resolved.context.name;
			const apply_result applied = apply_context(context_uri, displayName,
				resolved.context.revision, resolved.tracks, force);
			if (!applied.valid) {
				if (done) done(false, applied.error, pfc::infinite_size);
				return;
			}

			playlist_manager::ptr manager = playlist_manager::get();
			manager->set_active_playlist(applied.playlist);
			if (play && manager->playlist_get_item_count(applied.playlist) > 0) {
				manager->playlist_execute_default_action(applied.playlist, 0);
			}
			if (done) done(true, std::string(), applied.playlist);
		});
	});
}

void add_context_to_active_async(const std::string& context_uri, operation_done done,
	cancel_token cancel) {
	fb2k::splitTask([context_uri, done, cancel] {
		std::vector<track_info> tracks;
		std::string error;
		try {
			ensure_bridge_started(fb2k::mainAborter());
			bridge_client client;
			tracks = client.resolve(context_uri, cancel);
			if (tracks.empty()) error = "Spotify: no tracks found";
		} catch (const cancelled&) {
			return;
		} catch (const std::exception& failure) {
			error = user_error_message(failure);
		}

		fb2k::inMainThread([tracks, done, error] {
			if (!error.empty()) {
				if (done) done(false, error, pfc::infinite_size);
				return;
			}
			insert_tracks_into_active(tracks);
			if (done) done(true, std::string(), playlist_manager::get()->get_active_playlist());
		});
	});
}

void sync_all_tracked_async() {
	static std::atomic<bool> running{ false };
	bool expected = false;
	if (!running.compare_exchange_strong(expected, true)) return;

	const std::vector<tracked_playlist> tracked = list_tracked_playlists();
	if (tracked.empty()) {
		running = false;
		return;
	}

	fb2k::splitTask([tracked] {
		struct outcome {
			tracked_playlist target;
			resolve_result resolved;
			std::string error;
		};
		std::vector<outcome> outcomes;
		outcomes.reserve(tracked.size());
		for (const tracked_playlist& target : tracked) {
			outcome result;
			result.target = target;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				result.resolved = client.resolve_context(target.context_uri, cancel_token());
			} catch (const std::exception& failure) {
				result.error = user_error_message(failure);
			} catch (...) {
				result.error = "Spotify: unexpected error while syncing";
			}
			outcomes.push_back(std::move(result));
		}

		fb2k::inMainThread([outcomes] {
			size_t changed = 0;
			size_t failed = 0;
			for (const auto& outcome : outcomes) {
				if (!outcome.error.empty()) {
					++failed;
					console::print("foo_librespot: sync failed for ", outcome.target.context_uri.c_str(),
						": ", outcome.error.c_str());
					continue;
				}
				// The playlist may have been removed or detached while the
				// worker was running; find it again by context.
				if (find_playlist_by_context(outcome.target.context_uri) == pfc::infinite_size) continue;
				const apply_result applied = apply_context(outcome.target.context_uri,
					outcome.resolved.context.name, outcome.resolved.context.revision,
					outcome.resolved.tracks, false);
				if (!applied.valid) {
					++failed;
					continue;
				}
				if (applied.changed) ++changed;
			}
			console::print("foo_librespot: synced ", std::to_string(outcomes.size()).c_str(),
				" Spotify playlist(s), ", std::to_string(changed).c_str(), " changed",
				failed == 0 ? "" : ", some failed (see above)");
			running = false;
		});
	});
}

} // namespace flp
