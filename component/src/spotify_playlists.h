// Synced Spotify playlists: the foobar2000 playlist <-> Spotify context link.
//
// A playlist is "tracked" when it carries the persistent property
// foo_librespot.context (GUID below) whose value is the context URI
// (spotify:playlist:ID, spotify:album:ID, spotify:user:<user>:collection).
// Renaming the playlist does not break the link.
//
// All functions are main-thread only unless the name says *_async(); those
// resolve on a worker thread and call back on the main thread.

#pragma once

#include "bridge/protocol.h"

#include <SDK/playlist.h>

#include <functional>
#include <string>
#include <vector>

namespace flp {

// Persistent playlist properties. The first one is the documented
// "foo_librespot.context" key; playlist properties are GUIDs, so its value is
// this GUID.
extern const GUID playlist_context_property;
extern const GUID playlist_revision_property;

struct tracked_playlist {
	t_size playlist = pfc::infinite_size;
	std::string context_uri;
	// The library item revision the playlist was last checked against (see
	// sync_all_tracked_async); empty if never.
	std::string list_revision;
};

struct apply_result {
	bool valid = false;   // the playlist exists (created if necessary)
	bool changed = false; // items were added, removed or reordered
	bool created = false; // a new playlist was created
	t_size playlist = pfc::infinite_size;
	size_t added = 0;
	size_t removed = 0;
	std::string name;
	std::string error; // set when the apply failed (valid=false)
};

// "Spotify · <name>" using the configured prefix.
std::string playlist_display_name(const std::string& name);

// Main thread. Reading and writing the context/revision properties.
bool playlist_context(t_size playlist, std::string& out);
bool playlist_revision(t_size playlist, std::string& out);
bool set_playlist_context(t_size playlist, const std::string& context_uri);
bool set_playlist_revision(t_size playlist, const std::string& revision);

// Main thread. Synced playlists carry a playlist_lock that refuses adding,
// removing, reordering and replacing items, so local edits are not silently
// reverted by the next sync. Component code that has to change a synced
// playlist (the sync itself, Remove from Spotify playlist) does so while a
// synced_playlist_edit object is alive:
//
//     {
//         flp::synced_playlist_edit allow;
//         manager->playlist_remove_items(playlist, mask);
//     }
//
// Scopes nest; main thread only, like every playlist_manager call.
class synced_playlist_edit {
public:
	synced_playlist_edit();
	~synced_playlist_edit();
	synced_playlist_edit(const synced_playlist_edit&) = delete;
	synced_playlist_edit& operator=(const synced_playlist_edit&) = delete;
};

// True while a synced_playlist_edit is alive.
bool synced_playlist_edit_allowed();

// Main thread. Installs the lock on every synced (non-static) playlist that
// does not have it yet; called at startup.
void install_sync_locks();

// Main thread. Whether `playlist` is a synced playlist whose lock is ours.
bool is_sync_locked(t_size playlist);

// Main thread. Static playlists (radio, artist top tracks) are one-off
// snapshots: they keep a context key so reopening reuses the playlist, but the
// sync timer and "Sync now" skip them.
bool is_static_playlist(t_size playlist);
void set_static_playlist(t_size playlist);

t_size find_playlist_by_context(const std::string& context_uri);
std::vector<tracked_playlist> list_tracked_playlists();

// Main thread. Creates the playlist when needed and applies a minimal diff.
// `revision` may be empty (then the diff is always applied). `force` ignores
// a matching revision.
apply_result apply_context(const std::string& context_uri, const std::string& name,
	const std::string& revision, const std::vector<track_info>& tracks, bool force);

// The reusable playlist that single tracks, search tracks, artist top tracks
// and radio open into ("Spotify · Browse"). Its contents are replaced each
// time; it is static (never synced) and not locked.
extern const char* const browse_context_key;

// Main thread. Removes the sync property and the lock (the playlist stays).
bool detach_playlist(t_size playlist);
bool detach_active_playlist();

// Main thread. Appends resolved tracks to the active playlist (creating one if
// there is none), with preloaded info. Returns the number of tracks added; 0
// with `error` set when the active playlist refused them (a synced playlist).
size_t insert_tracks_into_active(const std::vector<track_info>& tracks, std::string* error = nullptr);

// Result of an asynchronous open/add operation. Always called on the main
// thread. `message` is empty on success, otherwise user-facing.
using operation_done = std::function<void(bool ok, const std::string& message, t_size playlist)>;

// Resolves `context_uri` on a worker thread, then on the main thread syncs the
// tracked playlist, activates it and (when `play`) starts its first track.
// A single track (spotify:track:) opens into the browse playlist instead of a
// synced playlist of its own. `done` may be empty. Every worker below also
// stops when foobar2000 quits, whatever `cancel` says.
void open_context_async(const std::string& context_uri, const std::string& name, bool play,
	bool force, operation_done done = operation_done(), cancel_token cancel = cancel_token());

// Resolves `context_uri` and appends the tracks to the active playlist.
void add_context_to_active_async(const std::string& context_uri, operation_done done,
	cancel_token cancel = cancel_token());

// Runs `fetch` on a worker thread, then on the main thread replaces the
// contents of the browse playlist (browse_context_key, created when missing)
// with the result and activates it. Used for radio, artist top tracks and
// single tracks. `key` only names the snapshot in messages; every snapshot
// shares the one browse playlist. `play` starts its first track.
void open_snapshot_async(const std::string& key, const std::string& name,
	std::function<resolve_result(bridge_client&, const cancel_token&)> fetch,
	operation_done done = operation_done(), cancel_token cancel = cancel_token(),
	bool play = false);

// Resolves every tracked playlist on a worker thread and applies changes.
// Safe to call from the main thread; ignores overlapping calls.
// `check_revisions_first` (the timer) reads the library once and skips the
// resolve of every playlist whose library revision has not changed since the
// last check; otherwise (menu, after a write) every playlist is resolved.
void sync_all_tracked_async(bool check_revisions_first = false);

// `cancel`, also aborting when foobar2000 quits.
cancel_token with_app_cancel(cancel_token cancel);

} // namespace flp
