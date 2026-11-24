#include "stdafx.h"
#include "liked_cache.h"

#include "bridge/process.h"
#include "component_helpers.h"

#include <SDK/metadb_display_field_provider.h>

#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <thread>
#include <unordered_set>

namespace flp {
namespace {

constexpr long long kFreshMs = 10 * 60 * 1000;    // re-read after 10 minutes
constexpr long long kRetryMs = 60 * 1000;         // after a failure
constexpr long long kSoonMs = 0;                  // refresh requested

long long now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Static storage only, constructed without SDK services (safe during DLL
// load); SDK calls happen on the worker and the main thread.
std::shared_mutex g_mutex;
std::unordered_set<std::string> g_liked;
std::atomic<long long> g_next_refresh_ms{0}; // when the set should be read (again)
std::atomic<bool> g_loading{false};
std::atomic<unsigned> g_generation{0};       // bumped by note/clear, so a slow read
											 // does not overwrite newer facts

// Tells foobar2000 that the field changed for these tracks (main thread).
void dispatch_changed(std::vector<std::string> uris) {
	if (uris.empty() || app_is_quitting()) return;
	fb2k::inMainThread([uris = std::move(uris)] {
		if (app_is_quitting()) return;
		auto db = metadb::get();
		metadb_handle_list handles;
		handles.prealloc(uris.size());
		for (const std::string& uri : uris) handles.add_item(db->handle_create(uri.c_str(), 0));
		metadb_io::get()->dispatch_refresh(handles);
	});
}

void load() {
	const unsigned generation = g_generation.load();
	std::vector<std::string> uris;
	bool ok = false;
	try {
		init_bridge_paths();
		const cancel_token cancel = app_cancel_token();
		bridge_process::instance().ensure_running(cancel);
		uris = bridge_client().liked(cancel);
		ok = true;
	} catch (const bridge_error& error) {
		// Not logged in: nothing is liked. The refresh after a login fills it.
		if (error.code == "not_logged_in") ok = true;
	} catch (const std::exception&) {
	}

	std::vector<std::string> changed;
	if (ok && generation == g_generation.load()) {
		std::unordered_set<std::string> fresh(uris.begin(), uris.end());
		std::unique_lock<std::shared_mutex> lock(g_mutex);
		for (const std::string& uri : g_liked) {
			if (fresh.count(uri) == 0) changed.push_back(uri);
		}
		for (const std::string& uri : fresh) {
			if (g_liked.count(uri) == 0) changed.push_back(uri);
		}
		g_liked = std::move(fresh);
	}
	g_next_refresh_ms = now_ms() + (ok ? kFreshMs : kRetryMs);
	g_loading = false;
	dispatch_changed(std::move(changed));
}

// Starts a read when one is due. Cheap when not: one atomic load. Never calls
// foobar2000 APIs itself, so it may run inside process_field.
void maybe_refresh() {
	if (now_ms() < g_next_refresh_ms.load() || app_is_quitting()) return;
	if (g_loading.exchange(true)) return;
	try {
		std::thread(load).detach();
	} catch (const std::exception&) {
		g_loading = false;
	}
}

bool contains(const char* path) {
	if (!is_spotify_track_path(path)) return false;
	maybe_refresh();
	std::shared_lock<std::shared_mutex> lock(g_mutex);
	return g_liked.count(path) != 0;
}

class liked_field_provider : public metadb_display_field_provider {
public:
	t_uint32 get_field_count() override { return 1; }

	void get_field_name(t_uint32 index, pfc::string_base& out) override {
		if (index == 0) out = "spotify_liked";
	}

	// %spotify_liked% is 1 for a Spotify track in Liked Songs, empty (false)
	// otherwise, so [%spotify_liked%] and $if(%spotify_liked%,...) work.
	bool process_field(t_uint32 index, metadb_handle* handle, titleformat_text_out* out) override {
		if (index != 0 || handle == nullptr) return false;
		if (!contains(handle->get_path())) return false;
		out->write(titleformat_inputtypes::unknown, "1");
		return true;
	}
};

FB2K_SERVICE_FACTORY(liked_field_provider);

// The library read (Spotify tab) is a good moment to pick up likes made
// elsewhere. Registration is a plain function pointer: no SDK at load time.
const bool g_library_hook = (set_library_read_hook(&liked_cache_refresh_async), true);

} // namespace

void liked_cache_note(const std::vector<std::string>& uris, bool saved) {
	std::vector<std::string> changed;
	{
		std::unique_lock<std::shared_mutex> lock(g_mutex);
		++g_generation;
		for (const std::string& uri : uris) {
			if (!is_spotify_track_uri(uri)) continue;
			const bool had = g_liked.count(uri) != 0;
			if (saved && !had) {
				g_liked.insert(uri);
				changed.push_back(uri);
			} else if (!saved && had) {
				g_liked.erase(uri);
				changed.push_back(uri);
			}
		}
	}
	dispatch_changed(std::move(changed));
}

void liked_cache_refresh_async() {
	g_next_refresh_ms = kSoonMs;
	maybe_refresh();
}

void liked_cache_clear() {
	std::vector<std::string> changed;
	{
		std::unique_lock<std::shared_mutex> lock(g_mutex);
		++g_generation;
		changed.assign(g_liked.begin(), g_liked.end());
		g_liked.clear();
	}
	g_next_refresh_ms = now_ms() + kRetryMs;
	dispatch_changed(std::move(changed));
}

} // namespace flp
