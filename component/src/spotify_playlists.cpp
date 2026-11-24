#include "stdafx.h"

#include "spotify_playlists.h"

#include "component_helpers.h"
#include "config.h"
#include "sync_plan.h"

#include "bridge/process.h"

#include <SDK/console.h>
#include <SDK/initquit.h>
#include <SDK/metadb.h>
#include <SDK/popup_message.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>

namespace flp {
namespace {

// "foo_librespot.context": the Spotify context URI of a synced playlist.
const GUID kPlaylistContextGuid = { 0x7c2f5a41, 0x0d63, 0x4b9e, { 0x8a, 0x31, 0x5f, 0xc4, 0x72, 0x09, 0xbe, 0x56 } };
// "foo_librespot.revision": the context revision last applied.
const GUID kPlaylistRevisionGuid = { 0x1e94b7d0, 0x6a58, 0x4f21, { 0xb3, 0x47, 0xd8, 0x0c, 0x95, 0x62, 0x1f, 0xa8 } };

// "foo_librespot.static": set on playlists that are a one-off snapshot (radio,
// artist top tracks). The sync timer and "Sync now" skip them.
const GUID kPlaylistStaticGuid = { 0x93d6e0a2, 0x4c17, 0x4f8b, { 0x9a, 0x5e, 0x21, 0x68, 0xb0, 0x3d, 0xc7, 0x4f } };

// "foo_librespot.name": the Spotify name last applied, so a rename on Spotify
// can rename the foobar2000 playlist without overriding a local rename.
const GUID kPlaylistNameGuid = { 0x4b2e8f17, 0xc6a3, 0x4d59, { 0x8e, 0x02, 0x7a, 0xd1, 0x39, 0x5c, 0xf4, 0x86 } };

// "foo_librespot.list_revision": the library item revision the playlist was
// last checked against by the sync timer.
const GUID kPlaylistListRevisionGuid = { 0xd57a0c93, 0x18e4, 0x4a6f, { 0xb9, 0x3d, 0x62, 0xf0, 0x4e, 0x81, 0xa5, 0x2c } };

constexpr char kBrowseContext[] = "foo_librespot:browse";
constexpr char kBrowseName[] = "Browse";

// ---- playlist lock ---------------------------------------------------------

int g_edit_depth = 0; // main thread only

void notify_locked_edit();

class sync_lock : public playlist_lock {
public:
	explicit sync_lock(t_size index) : m_index(index) {}

	t_size index() const { return m_index; }

	bool query_items_add(t_size, const pfc::list_base_const_t<metadb_handle_ptr>&,
		const bit_array&) override {
		return allow();
	}
	bool query_items_reorder(const t_size*, t_size) override { return allow(); }
	bool query_items_remove(const bit_array&, bool force) override { return force || allow(); }
	bool query_item_replace(t_size, const metadb_handle_ptr&, const metadb_handle_ptr&) override {
		return allow();
	}
	bool query_playlist_rename(const char*, t_size) override { return true; }
	bool query_playlist_remove() override { return true; }
	bool execute_default_action(t_size) override { return false; }
	void on_playlist_index_change(t_size index) override { m_index = index; }
	void on_playlist_remove() override;
	void get_lock_name(pfc::string_base& out) override { out = "Synced with Spotify"; }
	void show_ui() override { show_locked_message(); }
	t_uint32 get_filter_mask() override {
		return filter_add | filter_remove | filter_reorder | filter_replace;
	}

	static void show_locked_message() {
		popup_message::g_show(
			"This playlist is synced with Spotify, so it cannot be edited here: a sync would "
			"undo the change.\n\n"
			"Detach the playlist to edit it (right-click it > Detach from Spotify, or "
			"Edit > Detach from Spotify), or use Spotify > Remove from Spotify playlist to "
			"remove tracks on Spotify itself.",
			"foo_librespot", popup_message::icon_information);
	}

private:
	static bool allow() {
		if (g_edit_depth > 0) return true;
		notify_locked_edit();
		return false;
	}

	t_size m_index;
};

std::vector<service_ptr_t<sync_lock>>& installed_locks() {
	static std::vector<service_ptr_t<sync_lock>> locks;
	return locks;
}

void sync_lock::on_playlist_remove() {
	auto& locks = installed_locks();
	locks.erase(std::remove_if(locks.begin(), locks.end(),
					[this](const service_ptr_t<sync_lock>& lock) { return lock.get_ptr() == this; }),
		locks.end());
}

// A refused edit explains itself once in a while (not once per item; one drag
// can query the lock several times). Deferred: the query runs inside a
// playlist_manager call.
void notify_locked_edit() {
	static ULONGLONG last = 0;
	const ULONGLONG now = GetTickCount64();
	if (last != 0 && now - last < 10000) return;
	last = now;
	fb2k::inMainThread([] { sync_lock::show_locked_message(); });
}

sync_lock* find_lock(t_size playlist) {
	for (const auto& lock : installed_locks()) {
		if (lock->index() == playlist) return lock.get_ptr();
	}
	return nullptr;
}

void install_lock(t_size playlist) {
	if (playlist == pfc::infinite_size || find_lock(playlist) != nullptr) return;
	playlist_manager::ptr manager = playlist_manager::get();
	if (manager->playlist_lock_is_present(playlist)) return; // someone else's lock
	service_ptr_t<sync_lock> lock = new service_impl_t<sync_lock>(playlist);
	if (manager->playlist_lock_install(playlist, lock)) installed_locks().push_back(lock);
}

void remove_lock(t_size playlist) {
	sync_lock* lock = find_lock(playlist);
	if (lock == nullptr) return;
	service_ptr_t<sync_lock> keep = lock;
	playlist_manager::get()->playlist_lock_uninstall(playlist, keep);
	auto& locks = installed_locks();
	locks.erase(std::remove(locks.begin(), locks.end(), keep), locks.end());
}

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
const char* const browse_context_key = kBrowseContext;

synced_playlist_edit::synced_playlist_edit() { ++g_edit_depth; }
synced_playlist_edit::~synced_playlist_edit() { --g_edit_depth; }

bool synced_playlist_edit_allowed() { return g_edit_depth > 0; }

cancel_token with_app_cancel(cancel_token cancel) {
	cancel_token out;
	const std::function<bool()> inner = cancel.is_aborting;
	out.is_aborting = [inner] { return app_is_quitting() || (inner && inner()); };
	return out;
}

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

bool is_static_playlist(t_size playlist) {
	std::string value;
	return get_property(playlist, kPlaylistStaticGuid, value) && !value.empty();
}

void set_static_playlist(t_size playlist) {
	set_property(playlist, kPlaylistStaticGuid, "1");
	remove_lock(playlist);
}

bool is_sync_locked(t_size playlist) { return find_lock(playlist) != nullptr; }

void install_sync_locks() {
	for (const tracked_playlist& tracked : list_tracked_playlists()) install_lock(tracked.playlist);
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
		if (is_static_playlist(i)) continue;
		tracked_playlist entry{ i, stored, std::string() };
		get_property(i, kPlaylistListRevisionGuid, entry.list_revision);
		out.push_back(std::move(entry));
	}
	return out;
}

namespace {

// Renames a synced playlist after a rename on Spotify. A playlist the user
// renamed locally keeps its name: it is only renamed while it still carries
// the name this component gave it (or, for playlists synced before the name
// was recorded, while it starts with the configured prefix).
void follow_spotify_name(t_size playlist, const std::string& name) {
	if (name.empty()) return;
	std::string stored;
	const bool hasStored = get_property(playlist, kPlaylistNameGuid, stored);
	if (hasStored && stored == name) return;

	playlist_manager::ptr manager = playlist_manager::get();
	pfc::string8 current;
	if (manager->playlist_get_name(playlist, current)) {
		const std::string wanted = playlist_display_name(name);
		bool ours = false;
		if (hasStored) {
			ours = current.get_ptr() == playlist_display_name(stored);
		} else {
			const std::string prefix = effective_playlist_prefix();
			ours = !prefix.empty() && std::string(current.get_ptr()).rfind(prefix, 0) == 0;
		}
		if (ours && wanted != current.get_ptr()) {
			manager->playlist_rename(playlist, wanted.c_str(), wanted.size());
		}
	}
	set_property(playlist, kPlaylistNameGuid, name);
}

// Applies a minimal diff that turns `playlist` into `tracks`. Main thread.
void apply_tracks(t_size playlist, const std::string& revision,
	const std::vector<track_info>& tracks, apply_result& result) {
	playlist_manager::ptr manager = playlist_manager::get();
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
		return;
	}

	preload_tracks(plan.insert, byUri);

	// The sync is the one writer a synced playlist's lock lets through.
	synced_playlist_edit allow;
	manager->playlist_undo_backup(playlist);

	if (!plan.remove.empty()) {
		bit_array_bittable mask(oldCount);
		for (const size_t index : plan.remove) {
			if (index < oldCount) mask.set(index, true);
		}
		if (!manager->playlist_remove_items(playlist, mask)) {
			result.error = "Spotify: the playlist is locked";
			return;
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
			return;
		}
	}

	// One playlist_insert_items per contiguous run (a first load is one run)
	// instead of one per track.
	for (const insert_run& run : group_insertions(plan.insert)) {
		metadb_handle_list handles;
		handles.prealloc(run.uris.size());
		for (const std::string& uri : run.uris) {
			handles.add_item(metadb::get()->handle_create(uri.c_str(), 0));
		}
		const t_size inserted = manager->playlist_insert_items(playlist, run.index, handles,
			bit_array_false());
		if (inserted == pfc::infinite_size) {
			result.error = "Spotify: the playlist is locked";
			return;
		}
	}

	set_playlist_revision(playlist, revision);
	result.changed = true;
}

// Main thread. Replaces the browse playlist's contents with `tracks`.
apply_result apply_browse(const std::vector<track_info>& tracks) {
	apply_result result;
	playlist_manager::ptr manager = playlist_manager::get();
	t_size playlist = find_playlist_by_context(kBrowseContext);
	if (playlist == pfc::infinite_size) {
		const std::string displayName = playlist_display_name(kBrowseName);
		playlist = manager->create_playlist(displayName.c_str(), ~0, ~0);
		if (playlist == pfc::infinite_size) {
			result.error = "Spotify: could not create the playlist";
			return result;
		}
		set_playlist_context(playlist, kBrowseContext);
		result.created = true;
	}
	// Static: never synced and never locked; the user may edit it freely.
	if (!is_static_playlist(playlist)) set_static_playlist(playlist);

	result.valid = true;
	result.playlist = playlist;
	result.name = kBrowseName;
	apply_tracks(playlist, std::string(), tracks, result);
	if (!result.error.empty()) result.valid = false;
	return result;
}

bool is_single_track_uri(const std::string& uri) { return uri.rfind("spotify:track:", 0) == 0; }

} // namespace

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
		if (!name.empty()) set_property(playlist, kPlaylistNameGuid, name);
		result.created = true;
	} else if (!is_static_playlist(playlist)) {
		follow_spotify_name(playlist, name);
	}
	if (!is_static_playlist(playlist)) install_lock(playlist);

	result.valid = true;
	result.playlist = playlist;
	result.name = name;

	// An unchanged revision means Spotify reports the same contents; leave the
	// playlist alone.
	std::string storedRevision;
	if (!force && !revision.empty() && get_property(playlist, kPlaylistRevisionGuid, storedRevision) &&
		storedRevision == revision) {
		return result;
	}

	apply_tracks(playlist, revision, tracks, result);
	return result;
}

bool detach_playlist(t_size playlist) {
	playlist_manager_v2::ptr manager = playlist_manager_v2::get();
	if (playlist == pfc::infinite_size) return false;
	remove_lock(playlist);
	const bool detached = manager->playlist_remove_property(playlist, kPlaylistContextGuid);
	manager->playlist_remove_property(playlist, kPlaylistRevisionGuid);
	manager->playlist_remove_property(playlist, kPlaylistStaticGuid);
	manager->playlist_remove_property(playlist, kPlaylistNameGuid);
	manager->playlist_remove_property(playlist, kPlaylistListRevisionGuid);
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

size_t insert_tracks_into_active(const std::vector<track_info>& tracks, std::string* error) {
	if (tracks.empty()) return 0;

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
	if (handles.get_count() == 0) return 0;
	hints->on_done();

	playlist_manager::ptr manager = playlist_manager::get();
	t_size playlist = manager->get_active_playlist();
	if (playlist == pfc::infinite_size) {
		playlist = manager->create_playlist_autoname();
		if (playlist == pfc::infinite_size) return 0;
		manager->set_active_playlist(playlist);
	}
	const t_size base = manager->playlist_get_item_count(playlist);
	if (manager->playlist_insert_items(playlist, base, handles, bit_array_true()) ==
		pfc::infinite_size) {
		if (error != nullptr) {
			*error = is_sync_locked(playlist)
				? "Spotify: the active playlist is synced with Spotify and cannot be edited; "
				  "detach it first or pick another playlist"
				: "Spotify: the active playlist is locked";
		}
		return 0;
	}
	return handles.get_count();
}

void open_context_async(const std::string& context_uri, const std::string& name, bool play,
	bool force, operation_done done, cancel_token cancel) {
	if (is_single_track_uri(context_uri)) {
		// A lone track is not worth a synced playlist of its own.
		open_snapshot_async(context_uri, name,
			[context_uri](bridge_client& client, const cancel_token& ct) {
				return client.resolve_context(context_uri, ct);
			},
			done, cancel, play);
		return;
	}

	cancel = with_app_cancel(cancel);
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
		if (app_is_quitting()) return;

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
	cancel = with_app_cancel(cancel);
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
		if (app_is_quitting()) return;

		fb2k::inMainThread([tracks, done, error] {
			if (!error.empty()) {
				if (done) done(false, error, pfc::infinite_size);
				return;
			}
			std::string insertError;
			if (insert_tracks_into_active(tracks, &insertError) == 0 && !insertError.empty()) {
				if (done) done(false, insertError, pfc::infinite_size);
				return;
			}
			if (done) done(true, std::string(), playlist_manager::get()->get_active_playlist());
		});
	});
}

void open_snapshot_async(const std::string& key, const std::string& name,
	std::function<resolve_result(bridge_client&, const cancel_token&)> fetch,
	operation_done done, cancel_token cancel, bool play) {
	(void)key;
	(void)name;
	cancel = with_app_cancel(cancel);
	fb2k::splitTask([fetch, done, cancel, play] {
		resolve_result resolved;
		std::string error;
		try {
			ensure_bridge_started(fb2k::mainAborter());
			bridge_client client;
			resolved = fetch(client, cancel);
			if (resolved.tracks.empty()) error = "Spotify: no tracks found";
		} catch (const cancelled&) {
			return;
		} catch (const std::exception& failure) {
			error = user_error_message(failure);
		}
		if (app_is_quitting()) return;

		fb2k::inMainThread([done, resolved, error, play] {
			if (!error.empty()) {
				if (done) done(false, error, pfc::infinite_size);
				return;
			}
			const apply_result applied = apply_browse(resolved.tracks);
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

void sync_all_tracked_async(bool check_revisions_first) {
	static std::atomic<bool> running{ false };
	bool expected = false;
	if (!running.compare_exchange_strong(expected, true)) return;

	const std::vector<tracked_playlist> tracked = list_tracked_playlists();
	if (tracked.empty() || app_is_quitting()) {
		running = false;
		return;
	}

	fb2k::splitTask([tracked, check_revisions_first] {
		struct outcome {
			tracked_playlist target;
			resolve_result resolved;
			std::string error;
			bool skipped = false;      // the library revision did not change
			std::string list_revision; // library item revision to remember
			std::string list_name;     // library item name (for renames)
		};
		const cancel_token cancel = app_cancel_token();
		std::vector<outcome> outcomes;
		outcomes.reserve(tracked.size());

		// One library read answers the revision of every playlist and of Liked
		// Songs, instead of resolving each of them again.
		std::map<std::string, std::pair<std::string, std::string>> listed; // uri -> revision, name
		if (check_revisions_first) {
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				for (const library_section& section : client.library(cancel)) {
					// Recently played (and anything else the bridge describes
					// from its item cache) keeps the revision it first saw.
					if (!section_revisions_are_current(section.id)) continue;
					for (const library_item& item : section.items) {
						if (item.uri.empty()) continue;
						listed.emplace(item.uri, std::make_pair(item.revision, item.name));
					}
				}
			} catch (const cancelled&) {
				// Quitting. Not a std::exception: it must not leave the task.
				running = false;
				return;
			} catch (const std::exception&) {
				listed.clear(); // fall back to resolving everything
			}
		}

		for (const tracked_playlist& target : tracked) {
			if (app_is_quitting()) {
				running = false;
				return;
			}
			outcome result;
			result.target = target;
			const auto found = listed.find(target.context_uri);
			if (found != listed.end()) {
				result.list_revision = found->second.first;
				result.list_name = found->second.second;
				if (!result.list_revision.empty() && result.list_revision == target.list_revision) {
					result.skipped = true;
					outcomes.push_back(std::move(result));
					continue;
				}
			}
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				result.resolved = client.resolve_context(target.context_uri, cancel);
			} catch (const cancelled&) {
				running = false;
				return;
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
			size_t skipped = 0;
			for (const auto& outcome : outcomes) {
				if (!outcome.error.empty()) {
					++failed;
					console::print("foo_librespot: sync failed for ", outcome.target.context_uri.c_str(),
						": ", outcome.error.c_str());
					continue;
				}
				// The playlist may have been removed or detached while the
				// worker was running; find it again by context.
				const t_size playlist = find_playlist_by_context(outcome.target.context_uri);
				if (playlist == pfc::infinite_size) continue;
				if (outcome.skipped) {
					++skipped;
					follow_spotify_name(playlist, outcome.list_name);
					continue;
				}
				const apply_result applied = apply_context(outcome.target.context_uri,
					outcome.resolved.context.name.empty() ? outcome.list_name
														  : outcome.resolved.context.name,
					outcome.resolved.context.revision, outcome.resolved.tracks, false);
				if (!applied.valid || !applied.error.empty()) {
					++failed;
					continue;
				}
				if (applied.changed) ++changed;
				if (!outcome.list_revision.empty()) {
					set_property(applied.playlist, kPlaylistListRevisionGuid, outcome.list_revision);
				}
			}
			const std::string skippedText = skipped == 0
				? std::string()
				: " (" + std::to_string(skipped) + " unchanged on Spotify, not re-read)";
			console::print("foo_librespot: synced ", std::to_string(outcomes.size()).c_str(),
				" Spotify playlist(s), ", std::to_string(changed).c_str(), " changed",
				skippedText.c_str(), failed == 0 ? "" : ", some failed (see above)");
			running = false;
		});
	});
}

namespace {

// Locks are not persistent: put them back on the synced playlists at startup.
class sync_lock_initquit : public initquit {
public:
	void on_init() override { install_sync_locks(); }
};

static initquit_factory_t<sync_lock_initquit> g_sync_lock_initquit_factory;

} // namespace

} // namespace flp
