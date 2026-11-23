// Unit tests for the pure sync plan (src/sync_plan.cpp). No SDK, no host.
//
// Every case also applies the plan to an in-memory list and checks that the
// result equals the new URI list, which validates the remove -> reorder ->
// insert semantics used by spotify_playlists.cpp.

#include "../../src/sync_plan.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition) ++g_failures;
}

std::string join(const std::vector<std::string>& values) {
	std::string out;
	for (size_t i = 0; i < values.size(); ++i) {
		if (i != 0) out += ",";
		out += values[i];
	}
	return out;
}

std::vector<std::string> apply_plan(const std::vector<std::string>& old_uris,
	const flp::sync_plan& plan, const std::vector<std::string>& new_uris) {
	std::vector<std::string> items = old_uris;
	for (auto it = plan.remove.rbegin(); it != plan.remove.rend(); ++it) {
		items.erase(items.begin() + static_cast<std::ptrdiff_t>(*it));
	}

	if (!plan.survivor_order.empty()) {
		std::vector<std::string> reordered;
		reordered.reserve(items.size());
		for (const size_t old_index : plan.survivor_order) {
			const auto position = std::find(plan.survivors.begin(), plan.survivors.end(), old_index);
			if (position == plan.survivors.end()) {
				check(false, "survivor_order contains a removed index");
				return items;
			}
			const size_t current = static_cast<size_t>(position - plan.survivors.begin());
			reordered.push_back(items[current]);
		}
		items = reordered;
	}

	for (const flp::sync_insertion& insertion : plan.insert) {
		if (insertion.index > items.size()) {
			check(false, "insert index out of range");
			return items;
		}
		items.insert(items.begin() + static_cast<std::ptrdiff_t>(insertion.index), insertion.uri);
	}

	check(items == new_uris, "applying the plan produces the new list");
	if (items != new_uris) {
		std::printf("  old:        %s\n", join(old_uris).c_str());
		std::printf("  new:        %s\n", join(new_uris).c_str());
		std::printf("  resulting:  %s\n", join(items).c_str());
	}
	return items;
}

void expect_plan(const char* name, const std::vector<std::string>& old_uris,
	const std::vector<std::string>& new_uris, size_t remove_count, size_t insert_count,
	const std::vector<size_t>& expected_order,
	const std::vector<size_t>& protect = std::vector<size_t>()) {
	const flp::sync_plan plan = flp::make_sync_plan(old_uris, new_uris, protect);
	std::printf("-- %s\n", name);
	check(plan.remove.size() == remove_count, "remove count");
	check(plan.insert.size() == insert_count, "insert count");
	check(plan.survivor_order == expected_order, "survivor order");
	check(plan.result_count == new_uris.size(), "result count");
	apply_plan(old_uris, plan, new_uris);
}

void test_identical() {
	const std::vector<std::string> old = { "a", "b", "c" };
	const auto plan = flp::make_sync_plan(old, old);
	check(plan.no_change(), "identical list is a no-op");
	check(plan.remove.empty() && plan.insert.empty(), "no operations for identical list");
}

void test_inserts() {
	expect_plan("insert at end", { "a", "b" }, { "a", "b", "c" }, 0, 1, { 0, 1 });
	expect_plan("insert at front", { "a", "b" }, { "x", "a", "b" }, 0, 1, { 0, 1 });
	expect_plan("insert in the middle", { "a", "c" }, { "a", "b", "c" }, 0, 1, { 0, 1 });
	expect_plan("insert two", { "a" }, { "a", "b", "c" }, 0, 2, { 0 });
}

void test_removals() {
	expect_plan("remove last", { "a", "b", "c" }, { "a", "b" }, 1, 0, { 0, 1 });
	expect_plan("remove first", { "a", "b", "c" }, { "b", "c" }, 1, 0, { 1, 2 });
	expect_plan("remove middle", { "a", "b", "c" }, { "a", "c" }, 1, 0, { 0, 2 });
	expect_plan("remove all", { "a", "b" }, {}, 2, 0, {});
}

void test_reorders() {
	expect_plan("reverse", { "a", "b", "c" }, { "c", "b", "a" }, 0, 0, { 2, 1, 0 });
	expect_plan("rotate", { "a", "b", "c" }, { "b", "c", "a" }, 0, 0, { 1, 2, 0 });
	const auto plan = flp::make_sync_plan({ "a", "b" }, { "b", "a" });
	check(!plan.no_change(), "reorder is not a no-op");
}

void test_duplicates() {
	expect_plan("duplicates unchanged", { "a", "b", "a" }, { "a", "b", "a" }, 0, 0, { 0, 1, 2 });
	expect_plan("duplicate insert", { "a" }, { "a", "a" }, 0, 1, { 0 });
	expect_plan("duplicate removal", { "a", "a" }, { "a" }, 1, 0, { 0 });
	expect_plan("duplicate reorder", { "a", "b", "a" }, { "a", "a", "b" }, 0, 0, { 0, 2, 1 });
	expect_plan("duplicate plus new", { "a", "b" }, { "b", "a", "b" }, 0, 1, { 1, 0 });
}

void test_protected_duplicates() {
	// Old item 2 (the playing duplicate) must survive; item 0 is removed.
	expect_plan("protect playing duplicate", { "a", "b", "a" }, { "a", "b" }, 1, 0, { 2, 1 },
		{ 2 });
	const auto plan = flp::make_sync_plan({ "a", "b", "a" }, { "a", "b" }, { 2 });
	check(plan.remove.size() == 1 && plan.remove[0] == 0, "protected occurrence is kept");

	const auto stable = flp::make_sync_plan({ "a", "b", "a" }, { "a", "b", "a" }, { 2 });
	check(stable.no_change(), "playing duplicate with unchanged contents is a no-op");

	// The playing duplicate at the end survives while the equal one at the
	// front is removed; no reorder is needed.
	const auto shifted = flp::make_sync_plan({ "a", "a", "b" }, { "a", "b" }, { 1 });
	check(shifted.remove.size() == 1 && shifted.remove[0] == 0,
		"playing duplicate removal keeps order");
	check(shifted.survivor_order == shifted.survivors, "no reorder for equal duplicates");
}

void test_combined() {
	expect_plan("remove, reorder and insert", { "a", "b", "c", "d" }, { "d", "x", "b" }, 2, 1,
		{ 3, 1 });
	expect_plan("replace everything", { "a", "b" }, { "c", "d" }, 2, 2, {});
	expect_plan("empty old", {}, { "a", "b" }, 0, 2, {});
}

void test_ignores_invalid_protect_index() {
	const auto plan = flp::make_sync_plan({ "a" }, { "a" }, { 99 });
	check(plan.no_change(), "out-of-range protected index is ignored");
}

} // namespace

int main() {
	test_identical();
	test_inserts();
	test_removals();
	test_reorders();
	test_duplicates();
	test_protected_duplicates();
	test_combined();
	test_ignores_invalid_protect_index();

	if (g_failures != 0) {
		std::printf("%d sync plan test(s) FAILED\n", g_failures);
		return 1;
	}
	std::printf("All sync plan tests passed\n");
	return 0;
}
