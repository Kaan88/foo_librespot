#include "sync_plan.h"

#include <algorithm>
#include <map>

namespace flp {
namespace {

constexpr size_t kNone = static_cast<size_t>(-1);

struct uri_state {
	std::vector<size_t> old_positions;
	std::vector<size_t> new_positions;
	std::vector<size_t> protected_positions;
};

} // namespace

bool section_revisions_are_current(const std::string& section_id) {
	return section_id == "liked" || section_id == "made_for_you" || section_id == "playlists";
}

std::vector<insert_run> group_insertions(const std::vector<sync_insertion>& insertions) {
	std::vector<insert_run> runs;
	for (const sync_insertion& insertion : insertions) {
		if (!runs.empty() && insertion.index == runs.back().index + runs.back().uris.size()) {
			runs.back().uris.push_back(insertion.uri);
		} else {
			runs.push_back(insert_run{ insertion.index, { insertion.uri } });
		}
	}
	return runs;
}

sync_plan make_sync_plan(const std::vector<std::string>& old_uris,
	const std::vector<std::string>& new_uris, const std::vector<size_t>& protect_old) {
	sync_plan plan;
	plan.result_count = new_uris.size();

	std::vector<bool> old_protected(old_uris.size(), false);
	for (const size_t index : protect_old) {
		if (index < old_uris.size()) old_protected[index] = true;
	}

	std::map<std::string, uri_state> states;
	for (size_t i = 0; i < old_uris.size(); ++i) {
		states[old_uris[i]].old_positions.push_back(i);
	}
	for (size_t i = 0; i < new_uris.size(); ++i) {
		states[new_uris[i]].new_positions.push_back(i);
	}
	for (size_t i = 0; i < old_uris.size(); ++i) {
		if (old_protected[i]) states[old_uris[i]].protected_positions.push_back(i);
	}

	// Base matching: the k-th occurrence in the new list keeps the k-th
	// occurrence in the old list. This is the minimal-move matching and keeps
	// identical duplicates in place.
	std::vector<bool> old_used(old_uris.size(), false);
	std::vector<size_t> new_to_old(new_uris.size(), kNone);
	for (auto& entry : states) {
		uri_state& state = entry.second;
		const size_t pairs = std::min(state.old_positions.size(), state.new_positions.size());
		for (size_t k = 0; k < pairs; ++k) {
			old_used[state.old_positions[k]] = true;
			new_to_old[state.new_positions[k]] = state.old_positions[k];
		}
	}

	// Prefer a protected occurrence (the playing item) when a duplicate leaves
	// a choice: swap it into a matched slot of the same URI that is not itself
	// protected. The URI sequence is unchanged; only which instance survives.
	for (auto& entry : states) {
		const uri_state& state = entry.second;
		if (state.protected_positions.empty()) continue;
		const size_t pairs = std::min(state.old_positions.size(), state.new_positions.size());
		size_t protected_cursor = 0;
		for (size_t k = 0; k < pairs; ++k) {
			while (protected_cursor < state.protected_positions.size() &&
				old_used[state.protected_positions[protected_cursor]]) {
				++protected_cursor;
			}
			if (protected_cursor >= state.protected_positions.size()) break;

			const size_t matched = state.old_positions[k];
			if (old_protected[matched]) continue; // never evict another protected item
			const size_t preferred = state.protected_positions[protected_cursor];
			old_used[matched] = false;
			old_used[preferred] = true;
			new_to_old[state.new_positions[k]] = preferred;
			++protected_cursor;
		}
	}

	for (size_t i = 0; i < old_uris.size(); ++i) {
		if (old_used[i]) {
			plan.survivors.push_back(i);
		} else {
			plan.remove.push_back(i);
		}
	}

	plan.survivor_order.reserve(plan.survivors.size());
	for (size_t i = 0; i < new_uris.size(); ++i) {
		if (new_to_old[i] != kNone) {
			plan.survivor_order.push_back(new_to_old[i]);
		} else {
			plan.insert.push_back(sync_insertion{ i, new_uris[i] });
		}
	}

	return plan;
}

} // namespace flp
