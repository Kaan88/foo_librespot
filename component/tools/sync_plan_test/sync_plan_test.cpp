// Unit tests for the pure sync plan (src/sync_plan.cpp). No SDK, no host.
//
// Every case also applies the plan to an in-memory list and checks that the
// result equals the new URI list, which validates the remove -> reorder ->
// insert semantics used by spotify_playlists.cpp.

#include "../../src/error_policy.h"
#include "../../src/spotify_links.h"
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

	// Insert by contiguous runs, as spotify_playlists.cpp does.
	for (const flp::insert_run& run : flp::group_insertions(plan.insert)) {
		if (run.index > items.size()) {
			check(false, "insert index out of range");
			return items;
		}
		items.insert(items.begin() + static_cast<std::ptrdiff_t>(run.index), run.uris.begin(),
			run.uris.end());
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

void test_insert_runs() {
	// First load: everything is one run, i.e. one playlist_insert_items call.
	const auto fresh = flp::make_sync_plan({}, { "a", "b", "c", "d" });
	const auto freshRuns = flp::group_insertions(fresh.insert);
	check(freshRuns.size() == 1 && freshRuns[0].index == 0 && freshRuns[0].uris.size() == 4,
		"first load is a single contiguous run");

	const auto split = flp::make_sync_plan({ "b", "d" }, { "a", "b", "c", "d", "e", "f" });
	const auto runs = flp::group_insertions(split.insert);
	check(runs.size() == 3, "separated inserts become separate runs");
	check(runs.size() == 3 && runs[0].index == 0 && runs[1].index == 2 && runs[2].index == 4 &&
		runs[2].uris.size() == 2, "run positions and sizes");
	expect_plan("runs: interleaved", { "b", "d" }, { "a", "b", "c", "d", "e", "f" }, 0, 4, { 0, 1 });
	check(flp::group_insertions({}).empty(), "no insertions, no runs");
}

void test_links() {
	using flp::link_kind;
	const auto track = flp::parse_spotify_link("spotify:track:4uLU6hMCjMI75M1A2tKUQC");
	check(track.kind == link_kind::track && track.uri == "spotify:track:4uLU6hMCjMI75M1A2tKUQC",
		"spotify: track URI");
	const auto album = flp::parse_spotify_link("  spotify:album:4aawyAB9vmqN3uQ7FjRGTy  ");
	check(album.kind == link_kind::album && album.id == "4aawyAB9vmqN3uQ7FjRGTy", "album URI, trimmed");
	check(flp::parse_spotify_link("spotify:playlist:37i9dQZF1DXcBWIGoYBM5M").kind == link_kind::playlist,
		"playlist URI");
	check(flp::parse_spotify_link("spotify:artist:0OdUWJ0sBjDrqHygGUXeCF").kind == link_kind::artist,
		"artist URI");
	const auto legacy = flp::parse_spotify_link("spotify:user:someone:playlist:37i9dQZF1DXcBWIGoYBM5M");
	check(legacy.kind == link_kind::playlist && legacy.uri == "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M",
		"legacy user playlist URI is canonicalised");

	const auto url = flp::parse_spotify_link(
		"https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC?si=abcdef123");
	check(url.uri == "spotify:track:4uLU6hMCjMI75M1A2tKUQC", "track URL with ?si=");
	check(flp::parse_spotify_link("https://open.spotify.com/intl-de/album/4aawyAB9vmqN3uQ7FjRGTy").uri ==
		"spotify:album:4aawyAB9vmqN3uQ7FjRGTy", "URL with intl- prefix");
	check(flp::parse_spotify_link("http://OPEN.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M/").uri ==
		"spotify:playlist:37i9dQZF1DXcBWIGoYBM5M", "http, upper-case host, trailing slash");
	check(flp::parse_spotify_link("https://open.spotify.com/user/someone/playlist/37i9dQZF1DXcBWIGoYBM5M").uri ==
		"spotify:playlist:37i9dQZF1DXcBWIGoYBM5M", "URL with user prefix");
	check(flp::parse_spotify_link("https://open.spotify.com/artist/0OdUWJ0sBjDrqHygGUXeCF#x").kind ==
		link_kind::artist, "URL with fragment");

	check(!flp::parse_spotify_link("https://example.com/track/4uLU6hMCjMI75M1A2tKUQC"), "other host");
	check(!flp::parse_spotify_link("https://open.spotify.com.evil.com/track/abc"), "look-alike host");
	check(!flp::parse_spotify_link("https://open.spotify.com/show/abc"), "unsupported kind (show)");
	check(!flp::parse_spotify_link("spotify:track:"), "empty id");
	check(!flp::parse_spotify_link("spotify:track:ab/cd"), "bad characters in id");
	check(!flp::parse_spotify_link("C:\\music\\a.flac"), "local path");
	check(!flp::parse_spotify_link(""), "empty string");

	check(flp::spotify_web_url("spotify:track:4uLU6hMCjMI75M1A2tKUQC") ==
		"https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC", "web URL from URI");
	check(flp::spotify_web_url("nope").empty(), "web URL of a non-Spotify string is empty");
	check(flp::spotify_web_url("spotify:user:someone:collection") ==
			"https://open.spotify.com/collection/tracks",
		"web URL of Liked Songs");
	check(flp::spotify_web_url("spotify:user::collection").empty(),
		"web URL of a collection without a user is empty");
}

void test_error_policy() {
	using flp::describe_error;
	const auto kind = [](const char* code, const char* detail = "x", int retry = 0) {
		return describe_error(code, detail, retry);
	};

	const flp::error_info premium = kind("premium_required");
	check(premium.kind == "premium_required" && premium.popup && !premium.skip_track,
		"premium_required: popup, does not skip");
	check(premium.message.find("Premium") != std::string::npos, "premium_required message mentions Premium");

	const flp::error_info limited = kind("rate_limited", "x", 42);
	check(limited.kind == "rate_limited" && !limited.skip_track, "rate_limited: does not skip");
	check(limited.message.find("42 s") != std::string::npos, "rate_limited message carries retry-after");
	check(kind("rate_limited").message.find("in a minute") != std::string::npos,
		"rate_limited without retry-after says a minute");

	const flp::error_info region = kind("region_unavailable");
	check(region.skip_track && region.popup && region.message.find("country") != std::string::npos,
		"region_unavailable: skip the track, mention the country");
	const flp::error_info gone = kind("track_unavailable");
	check(gone.skip_track && gone.popup && gone.message.find("no longer available") != std::string::npos,
		"track_unavailable: skip the track");

	check(kind("not_logged_in").kind == "not_logged_in" && !kind("not_logged_in").skip_track,
		"not_logged_in keeps its message and does not skip");
	const flp::error_info connecting = describe_error("unavailable",
		"connecting to Spotify failed: dial tcp: i/o timeout", 0);
	check(connecting.kind == "connecting" && connecting.message.find("dial tcp") != std::string::npos &&
			!connecting.skip_track && !connecting.popup,
		"connecting failure keeps its reason (no popup, no skip)");
	const flp::error_info other = describe_error("internal", "boom", 0);
	check(other.kind == "other" && other.message == "Spotify: boom" && !other.skip_track,
		"unknown codes pass the bridge message through");
	check(describe_error("unavailable", "x", 0).kind == "other", "old 'unavailable' stays generic");

	const flp::bridge_error error("whatever", "region_unavailable", 0);
	check(describe_error(error).skip_track, "bridge_error maps by its code");
	check(describe_error(std::runtime_error("pipe broke")).message == "Spotify: pipe broke",
		"non-bridge errors are generic");

	flp::reset_reported_kinds();
	check(flp::first_report_of_kind("region_unavailable"), "first report of a kind raises a popup");
	check(!flp::first_report_of_kind("region_unavailable"), "the same kind again does not");
	check(flp::first_report_of_kind("premium_required"), "another kind does");
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
	test_insert_runs();
	test_links();
	test_error_policy();

	if (g_failures != 0) {
		std::printf("%d sync plan test(s) FAILED\n", g_failures);
		return 1;
	}
	std::printf("All sync plan tests passed\n");
	return 0;
}
