// The Liked Songs set behind %spotify_liked% (a metadb_display_field_provider).
//
// The set is read from the bridge (`liked`) on a worker thread the first time
// a field asks for it, again 10 minutes later, right after the Spotify library
// was read and after a logout; save/unsave update it at once. Whenever it
// changes, the affected tracks get metadb_io::dispatch_refresh on the main
// thread, so playlists redraw. All functions are thread-safe.

#pragma once

#include <string>
#include <vector>

namespace flp {

// Records the outcome of a save (true) or unsave (false) of `uris`.
void liked_cache_note(const std::vector<std::string>& uris, bool saved);

// Reads the set again soon (after a library read, a login).
void liked_cache_refresh_async();

// Forgets the set (logout); the refresh after the next login fills it again.
void liked_cache_clear();

} // namespace flp
