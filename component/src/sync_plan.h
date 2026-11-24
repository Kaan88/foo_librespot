// Minimal-diff computation for synced Spotify playlists.
//
// Pure data in, pure data out: no SDK or host dependency, so the whole
// algorithm is unit-tested by tools/sync_plan_test without foobar2000.
//
// The plan is applied in three steps:
//   1. remove the old items listed in `remove` (descending);
//   2. reorder the surviving items to `survivor_order` (old indices in the
//      order they must appear);
//   3. insert each `insert` item at its final index (ascending).
//
// Duplicate URIs are matched by occurrence count. `protect_old` lists old
// indices that should survive when a duplicate makes a choice possible (the
// playing item and the selection), so the playing item is never removed and
// re-inserted for a duplicate of another track.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace flp {

struct sync_insertion {
	size_t index = 0; // final position in the new list
	std::string uri;
};

struct sync_plan {
	// Old indices to remove, ascending.
	std::vector<size_t> remove;
	// Surviving old indices, ascending (the playlist after step 1).
	std::vector<size_t> survivors;
	// Surviving old indices in the order they must appear (step 2).
	std::vector<size_t> survivor_order;
	// New items to insert (step 3).
	std::vector<sync_insertion> insert;
	size_t result_count = 0;

	bool no_change() const {
		return remove.empty() && insert.empty() && survivor_order == survivors;
	}
};

// A run of new items that occupy consecutive final positions.
struct insert_run {
	size_t index = 0; // final position of the first item
	std::vector<std::string> uris;
};

// Groups plan.insert (ascending final indices) into contiguous runs. Applying
// the runs in order, each at its `index`, equals applying every insertion.
std::vector<insert_run> group_insertions(const std::vector<sync_insertion>& insertions);

// Whether the item revisions of library section `section_id` are read from
// Spotify on every library read, so the sync timer may skip a playlist whose
// revision did not change. True for Liked Songs and the rootlist sections
// ("liked", "made_for_you", "playlists"). The other sections (recently played)
// are described from the bridge's item cache, which keeps the revision it saw
// first: trusting it would skip a playlist that did change.
bool section_revisions_are_current(const std::string& section_id);

sync_plan make_sync_plan(const std::vector<std::string>& old_uris,
	const std::vector<std::string>& new_uris,
	const std::vector<size_t>& protect_old = std::vector<size_t>());

} // namespace flp
