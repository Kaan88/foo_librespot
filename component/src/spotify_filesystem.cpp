// A `filesystem` service for Spotify links.
//
// Why a filesystem service: it is the only SDK extension point that sits in
// the path foobar2000 itself uses to turn dropped / pasted / opened locations
// into playlist items (playlist_incoming_item_filter::process_locations):
//   1. filesystem::g_get_canonical_path()  -> every filesystem may rewrite the
//      path; https://open.spotify.com/track/ID?si=... becomes spotify:track:ID.
//   2. filesystem::g_get_interface()       -> picks the filesystem by is_our_path().
//   3. directory test + list_directory()   -> a location that is a "folder" is
//      expanded into its children. spotify:album:ID, spotify:playlist:ID and
//      spotify:artist:ID are folders whose children are spotify:track:ID files,
//      so albums and playlists expand to their tracks exactly like a directory.
//   4. spotify:track:ID is a "file"; the existing input_spotify claims it.
//
// playlist_incoming_item_filter itself is a host (core) service and cannot be
// replaced or hooked, and link_resolver only handles link *files* (.url/.lnk),
// so neither can accept a bare link; the filesystem service is the hook.
//
// The expansion runs on foobar2000's worker thread (inside its progress
// dialog), so blocking on the bridge is allowed there; abort_callback is
// honoured. Track metadata from the resolve reply is handed to the metadb as
// hints, so the new playlist items show titles at once instead of waiting for
// one `info` round trip per track.

#include "stdafx.h"

#include "component_helpers.h"
#include "spotify_links.h"

#include "bridge/process.h"

#include <SDK/metadb.h>

#include <set>

namespace {

constexpr uint32_t kFolderAttribs = t_filestats2::attr_folder | t_filestats2::attr_readonly |
	t_filestats2::attr_remote | t_filestats2::attr_network;
constexpr uint32_t kFileAttribs = t_filestats2::attr_readonly | t_filestats2::attr_remote |
	t_filestats2::attr_network;
constexpr uint32_t kAllAttribs = t_filestats2::attr_readonly | t_filestats2::attr_folder |
	t_filestats2::attr_hidden | t_filestats2::attr_system | t_filestats2::attr_remote |
	t_filestats2::attr_network;

t_filestats2 stats_for(const flp::spotify_link& link) {
	t_filestats2 stats;
	stats.m_attribsValid = kAllAttribs;
	stats.m_attribs = link.kind == flp::link_kind::track ? kFileAttribs : kFolderAttribs;
	return stats;
}

class spotify_filesystem : public filesystem_v4 {
public:
	bool get_canonical_path(const char* path, pfc::string_base& out) override {
		const flp::spotify_link link = flp::parse_spotify_link(path == nullptr ? "" : path);
		if (!link) return false;
		out = link.uri.c_str();
		return true;
	}

	bool is_our_path(const char* path) override {
		return static_cast<bool>(flp::parse_spotify_link(path == nullptr ? "" : path));
	}

	bool get_display_path(const char* path, pfc::string_base& out) override {
		const flp::spotify_link link = flp::parse_spotify_link(path == nullptr ? "" : path);
		if (!link) return false;
		out = link.uri.c_str();
		return true;
	}

	void open(service_ptr_t<file>&, const char*, t_open_mode, abort_callback&) override {
		// Audio never goes through a file object; input_spotify streams from the bridge.
		throw exception_io_denied();
	}
	void remove(const char*, abort_callback&) override { throw exception_io_denied(); }
	void move(const char*, const char*, abort_callback&) override { throw exception_io_denied(); }
	void move_overwrite(const char*, const char*, abort_callback&) override {
		throw exception_io_denied();
	}
	void create_directory(const char*, abort_callback&) override { throw exception_io_denied(); }
	void make_directory(const char*, abort_callback&, bool* = nullptr) override {
		throw exception_io_denied();
	}
	bool supports_content_types() override { return false; }
	char pathSeparator() override { return '/'; }

	t_filestats2 get_stats2(const char* path, uint32_t, abort_callback& abort) override {
		abort.check();
		const flp::spotify_link link = flp::parse_spotify_link(path == nullptr ? "" : path);
		if (!link) throw exception_io_not_found();
		return stats_for(link);
	}

	t_filestats2 getStatsOpportunist(const char* path) override {
		const flp::spotify_link link = flp::parse_spotify_link(path == nullptr ? "" : path);
		if (!link) {
			t_filestats2 stats;
			stats.m_attribsValid = t_filestats2::attr_remote;
			stats.m_attribs = t_filestats2::attr_remote;
			return stats;
		}
		return stats_for(link);
	}

	void list_directory_v3(const char* path, directory_callback_v3& callback, unsigned,
		abort_callback& abort) override {
		const flp::spotify_link link = flp::parse_spotify_link(path == nullptr ? "" : path);
		if (!link) throw exception_io_not_found();
		if (link.kind == flp::link_kind::track) throw exception_io_not_found();

		std::vector<flp::track_info> tracks;
		try {
			flp::ensure_bridge_started(abort);
			flp::bridge_client client;
			const flp::cancel_token cancel = flp::make_cancel(abort);
			if (link.kind == flp::link_kind::artist) {
				tracks = client.artist(link.uri, cancel).top_tracks;
			} else {
				tracks = client.resolve(link.uri, cancel);
			}
		} catch (const flp::cancelled&) {
			throw exception_aborted();
		} catch (const exception_aborted&) {
			throw;
		} catch (const std::exception& error) {
			flp::throw_io_error(error);
		}

		metadb_hint_list_v3::ptr hints = metadb_hint_list_v3::create();
		std::set<std::string> seen;
		t_filestats2 fileStats;
		fileStats.m_attribsValid = kAllAttribs;
		fileStats.m_attribs = kFileAttribs;
		for (const flp::track_info& track : tracks) {
			const std::string trackPath = flp::track_path(track);
			if (trackPath.empty() || !flp::is_spotify_track_uri(trackPath)) continue;
			// Playlists may list a track twice; keep both like Spotify does.
			if (seen.insert(trackPath).second) {
				metadb_handle_ptr handle = metadb::get()->handle_create(trackPath.c_str(), 0);
				file_info_impl info;
				flp::fill_file_info(track, info);
				hints->add_hint_forced(handle, info, t_filestats(), false);
			}
			if (!callback.on_entry(this, trackPath.c_str(), fileStats, abort)) {
				break;
			}
		}
		fb2k::inMainThread([hints] { hints->on_done(); });
	}
};

static service_factory_single_t<spotify_filesystem> g_spotify_filesystem_factory;

} // namespace
