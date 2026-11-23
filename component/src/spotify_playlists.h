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

// Main thread. Removes the sync property (the playlist stays).
bool detach_playlist(t_size playlist);
bool detach_active_playlist();

// Main thread. Appends resolved tracks to the active playlist (creating one if
// there is none), with preloaded info.
void insert_tracks_into_active(const std::vector<track_info>& tracks);

// Result of an asynchronous open/add operation. Always called on the main
// thread. `message` is empty on success, otherwise user-facing.
using operation_done = std::function<void(bool ok, const std::string& message, t_size playlist)>;

// Resolves `context_uri` on a worker thread, then on the main thread syncs the
// tracked playlist, activates it and (when `play`) starts its first track.
// `done` may be empty.
void open_context_async(const std::string& context_uri, const std::string& name, bool play,
	bool force, operation_done done = operation_done(), cancel_token cancel = cancel_token());

// Resolves `context_uri` and appends the tracks to the active playlist.
void add_context_to_active_async(const std::string& context_uri, operation_done done,
	cancel_token cancel = cancel_token());

// Runs `fetch` on a worker thread, then on the main thread opens the result as a
// static (never synced) playlist keyed by `key`, replacing its contents if it
// already exists, and activates it. Used for radio and artist top tracks.
void open_snapshot_async(const std::string& key, const std::string& name,
	std::function<resolve_result(bridge_client&, const cancel_token&)> fetch,
	operation_done done = operation_done(), cancel_token cancel = cancel_token());

// Resolves every tracked playlist on a worker thread and applies changes.
// Safe to call from the main thread; ignores overlapping calls.
void sync_all_tracked_async();

} // namespace flp
