// Fake librespot bridge used by the automated tests.
//
// Speaks proto/PROTOCOL.md v1 on a named pipe, generates a 440 Hz sine wave and
// accepts the same command line as the real helper (--parent-pid/--pipe), so it
// can be copied to a temp folder as librespot-bridge.exe and exercised through
// bridge_process (process spawn + pipe wait).

#include "../src/bridge/json.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct options_t {
	std::wstring pipe = L"foo_librespot";
	long long duration_ms = 30000;
	int rate = 44100;
	int channels = 2;
	bool realtime = false;
	bool logged_out = false;
	bool quiet = false;
	int cache_mb = 0; // --cache-mb at spawn; changed by the config op
	// v6: the owning process (--parent-pid), the version status reports
	// (--version, for the component's version gate) and debug logging.
	unsigned long parent_pid = 0;
	std::string version = "0.1.0-fake";
	bool debug = false;
	int play_reports = 0; // report_play calls accepted (see test_control)
} g_options;

// Fake audio cache: URIs opened while the cache is on come "from cache" the
// next time, so the component's handling of the v4 header field is testable.
std::mutex g_cache_mutex;
std::set<std::string> g_cached;


void log_line(const char* text) {
	if (!g_options.quiet) std::printf("[fake_bridge] %s\n", text);
}

std::string json_quote(const std::string& value) { return flp::json::quote(value); }

std::string make_track(const std::string& uri, int index = 0, int bitrate = 320);
// v2 test state: revision and track count change through the test-only
// "test_control" op, so tests can force a resync without restarting.
struct v2_state_t {
	std::mutex mutex;
	std::string revision = "1";
	int track_count = 3;
	// v3: Liked Songs membership and playlist_add bookkeeping.
	std::set<std::string> saved;
	int playlist_adds = 0;
	// v5: followed artists/albums/playlists (the library), the user's own
	// playlist and the playlists created through playlist_create.
	std::set<std::string> followed;
	std::string user_playlist_name = "Road Trip";
	bool user_playlist_present = true;
	std::vector<std::string> user_playlist_contents;
	std::vector<std::pair<std::string, std::string>> created_playlists;
	int playlist_edits = 0;
} g_v2;

std::string context_name(const std::string& uri) {
	if (uri.rfind("spotify:album:", 0) == 0) return "Fake Album";
	if (uri.rfind("spotify:user:", 0) == 0) return "Liked Songs";
	if (uri.rfind("spotify:artist:", 0) == 0) return "Fake Artist";
	return "Fake Playlist";
}

std::string make_item(const std::string& uri, const std::string& kind, const std::string& name,
	const std::string& subtitle, const std::string& revision, long long trackCount,
	int editable = -1) {
	std::string out = "{";
	out += "\"uri\":" + json_quote(uri);
	out += ",\"kind\":" + json_quote(kind);
	out += ",\"name\":" + json_quote(name);
	out += ",\"subtitle\":" + json_quote(subtitle);
	out += ",\"image_url\":null";
	out += ",\"track_count\":" + std::to_string(trackCount);
	out += ",\"revision\":";
	out += revision.empty() ? "null" : json_quote(revision);
	if (editable >= 0) out += editable != 0 ? ",\"editable\":true" : ",\"editable\":false";
	out += "}";
	return out;
}

std::string make_track_ids(int count) {
	std::string out = "[";
	for (int i = 0; i < count; ++i) {
		if (i != 0) out += ',';
		out += make_track("", i);
	}
	out += "]";
	return out;
}

std::string make_library() {
	std::lock_guard<std::mutex> lock(g_v2.mutex);
	const int count = g_v2.track_count;
	const std::string revision = g_v2.revision;

	std::string out = "{\"ok\":true,\"sections\":[";
	out += "{\"id\":\"liked\",\"title\":\"Liked Songs\",\"items\":[" +
		make_item("spotify:user:fake_user:collection", "collection", "Liked Songs", "", revision,
			count) +
		"]},";
	out += "{\"id\":\"made_for_you\",\"title\":\"Made for you\",\"items\":[";
	out += make_item("spotify:playlist:made000000000000000001", "playlist", "Daily Mix 1",
		"Spotify", revision, count);
	out += ",";
	out += make_item("spotify:playlist:made000000000000000002", "playlist", "Discover Weekly",
		"Spotify", revision, count);
	out += "]},";
	out += "{\"id\":\"playlists\",\"title\":\"Your playlists\",\"items\":[";
	bool first = true;
	const auto append = [&out, &first](const std::string& item) {
		if (!first) out += ",";
		first = false;
		out += item;
	};
	if (g_v2.user_playlist_present) {
		append(make_item("spotify:playlist:user000000000000000001", "playlist",
			g_v2.user_playlist_name, "fake_user", revision, count, 1));
	}
	for (const auto& created : g_v2.created_playlists) {
		append(make_item(created.first, "playlist", created.second, "fake_user", revision, 0, 1));
	}
	append(make_item("spotify:playlist:shared00000000000001", "playlist", "Shared Mix",
		"someone_else", revision, count, 0));
	out += "]},";
	out += "{\"id\":\"albums\",\"title\":\"Albums\",\"items\":[";
	out += make_item("spotify:album:alb0000000000000000001", "album", "Saved Album One",
		"Fake Artist", revision, count);
	out += ",";
	out += make_item("spotify:album:alb0000000000000000002", "album", "Saved Album Two",
		"Fake Artist", revision, count);
	out += "]},";
	out += "{\"id\":\"artists\",\"title\":\"Artists\",\"items\":["; 
	out += make_item("spotify:artist:art0000000000000000001", "artist", "Fake Artist", "",
		"", count);
	out += "]},";
	out += "{\"id\":\"recently_played\",\"title\":\"Recently played\",\"items\":[";
	out += make_item("spotify:album:recent0000000000000001", "album", "Recent Album",
		"Fake Artist", "", 0);
	out += ",";
	out += make_item("spotify:playlist:recent0000000000000001", "playlist", "Recent Playlist",
		"fake_user", "", 0);
	out += "]},";
	out += "{\"id\":\"top_tracks\",\"title\":\"Top tracks\",\"items\":[]},";
	out += "{\"id\":\"top_artists\",\"title\":\"Top artists\",\"items\":[]}";
	out += "]}";
	return out;
}

std::string make_search(const std::string& query, int limit) {
	if (limit <= 0) limit = 20;
	if (limit > 50) limit = 50;
	std::lock_guard<std::mutex> lock(g_v2.mutex);
	const std::string revision = g_v2.revision;

	const int trackCount = limit < 2 ? limit : 2;
	std::string out = "{\"ok\":true,\"tracks\":";
	out += make_track_ids(trackCount);
	out += ",\"albums\":[";
	if (limit > 0) {
		out += make_item("spotify:album:sea0000000000000000001", "album",
			query + " Album", "Fake Artist", revision, g_v2.track_count);
	}
	out += "],\"artists\":[";
	if (limit > 0) {
		out += make_item("spotify:artist:sea0000000000000000001", "artist",
			query + " Artist", "", "", g_v2.track_count);
	}
	out += "],\"playlists\":[";
	if (limit > 0) {
		out += make_item("spotify:playlist:sea0000000000000000001", "playlist",
			query + " Playlist", "fake_user", revision, g_v2.track_count);
	}
	out += "]}";
	return out;
}

std::string make_artist(const std::string& uri) {
	std::lock_guard<std::mutex> lock(g_v2.mutex);
	const std::string revision = g_v2.revision;
	std::string out = "{\"ok\":true,\"artist\":";
	out += make_item(uri, "artist", "Fake Artist", "", "", g_v2.track_count);
	out += ",\"top_tracks\":" + make_track_ids(2);
	out += ",\"albums\":[";
	out += make_item("spotify:album:art0000000000000000001", "album", "Artist Album One",
		"Fake Artist", revision, g_v2.track_count);
	out += ",";
	out += make_item("spotify:album:art0000000000000000002", "album", "Artist Album Two",
		"Fake Artist", revision, g_v2.track_count);
	out += "],\"singles\":[";
	out += make_item("spotify:album:art0000000000000000003", "album", "Artist Single",
		"Fake Artist", revision, g_v2.track_count);
	out += "]}";
	return out;
}


std::string make_track(const std::string& uri, int index, int bitrate) {
	std::string trackUri = uri;
	if (uri.rfind("spotify:track:", 0) != 0) {
		char id[64];
		std::snprintf(id, sizeof(id), "fake%018d", index);
		trackUri = std::string("spotify:track:") + id;
	}

	std::string out = "{";
	out += "\"uri\":" + json_quote(trackUri);
	out += ",\"title\":" + json_quote("Fake Track " + std::to_string(index + 1));
	out += ",\"artists\":[\"Fake Artist\"";
	if (index == 0) out += ",\"Second Artist\"";
	out += "]";
	out += ",\"album\":\"Fake Album\"";
	out += ",\"album_uri\":\"spotify:album:fakealbum000000000001\"";
	out += ",\"artist_uris\":[\"spotify:artist:fakeartist00000000001\"";
	if (index == 0) out += ",\"spotify:artist:secondartist0000000001\"";
	out += "]";
	out += ",\"album_artist\":\"Fake Artist\"";
	out += ",\"track_number\":" + std::to_string(index + 1);
	out += ",\"disc_number\":1";
	out += ",\"date\":\"2026\"";
	out += ",\"duration_ms\":" + std::to_string(g_options.duration_ms);
	out += ",\"codec\":\"vorbis\"";
	out += ",\"bitrate\":" + std::to_string(bitrate);
	out += ",\"cover_url\":\"\"";
	out += ",\"track_gain_db\":-7.25";
	out += ",\"track_peak\":0.9";
	// Tracks whose URI mentions "album" carry album gain (-9.5 dB, peak 0.95).
	if (uri.find("album") != std::string::npos) {
		out += ",\"album_gain_db\":-9.5";
		out += ",\"album_peak\":0.95";
	} else {
		out += ",\"album_gain_db\":null";
		out += ",\"album_peak\":null";
	}
	out += ",\"playable\":true";
	out += "}";
	return out;
}

bool write_all(HANDLE pipe, const void* data, size_t bytes) {
	const char* p = static_cast<const char*>(data);
	size_t left = bytes;
	while (left > 0) {
		DWORD written = 0;
		if (!WriteFile(pipe, p, static_cast<DWORD>(left), &written, nullptr) || written == 0) {
			return false;
		}
		p += written;
		left -= written;
	}
	return true;
}

bool write_line(HANDLE pipe, const std::string& line) {
	std::string withNewline = line;
	withNewline += '\n';
	return write_all(pipe, withNewline.data(), withNewline.size());
}

std::string error_reply(const char* code, const char* message, int retry_after = 0) {
	std::string out = std::string("{\"ok\":false,\"error\":") + json_quote(message) + ",\"code\":" +
		json_quote(code);
	if (retry_after > 0) out += ",\"retry_after\":" + std::to_string(retry_after);
	return out + "}";
}

// Test-only: a URI containing errpremium / errrate / errregion / errgone makes
// info and open fail with the matching v4 error code.
bool write_uri_error(HANDLE pipe, const std::string& uri) {
	if (uri.find("errpremium") != std::string::npos) {
		write_line(pipe, error_reply("premium_required", "Spotify Premium is required"));
	} else if (uri.find("errrate") != std::string::npos) {
		write_line(pipe, error_reply("rate_limited", "Spotify is rate limiting requests", 42));
	} else if (uri.find("errregion") != std::string::npos) {
		write_line(pipe, error_reply("region_unavailable", "This track is not available in your country"));
	} else if (uri.find("errgone") != std::string::npos) {
		write_line(pipe, error_reply("track_unavailable", "This track is no longer available"));
	} else {
		return false;
	}
	return true;
}

bool read_request_line(HANDLE pipe, std::string& out) {
	out.clear();
	char buffer[4096];
	for (;;) {
		DWORD got = 0;
		if (!ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) || got == 0) return false;
		out.append(buffer, got);
		const size_t pos = out.find('\n');
		if (pos != std::string::npos) {
			out.resize(pos);
			return true;
		}
		if (out.size() > 1024 * 1024) return false;
	}
}

void stream_pcm(HANDLE pipe, long long startFrame) {
	const long long totalFrames =
		static_cast<long long>(g_options.duration_ms * g_options.rate / 1000);
	const size_t framesPerChunk = 4096;
	std::vector<float> samples(framesPerChunk * static_cast<size_t>(g_options.channels));

	long long frame = startFrame;
	while (frame < totalFrames) {
		const size_t count = static_cast<size_t>(
			std::min<long long>(static_cast<long long>(framesPerChunk), totalFrames - frame));
		for (size_t i = 0; i < count; ++i) {
			const double t = static_cast<double>(frame + static_cast<long long>(i)) / g_options.rate;
			const float value = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t));
			for (int ch = 0; ch < g_options.channels; ++ch) {
				samples[i * static_cast<size_t>(g_options.channels) + static_cast<size_t>(ch)] = value;
			}
		}
		if (!write_all(pipe, samples.data(), count * static_cast<size_t>(g_options.channels) *
				sizeof(float))) {
			return; // client went away
		}
		frame += static_cast<long long>(count);
		if (g_options.realtime) {
			Sleep(static_cast<DWORD>(count * 1000 / g_options.rate));
		}
	}
}

void handle_connection(HANDLE pipe) {
	std::string line;
	if (!read_request_line(pipe, line)) return;

	flp::json::value request;
	std::string parseError;
	if (!flp::json::parse(line, request, parseError)) {
		write_line(pipe, error_reply("bad_request", parseError.c_str()));
		return;
	}

	const std::string op = request.get_string("op");
	const std::string uri = request.get_string("uri");
	if (!g_options.quiet) std::printf("[fake_bridge] op=%s\n", op.c_str());

	if (op == "status") {
		std::string reply = "{\"ok\":true,\"logged_in\":";
		reply += g_options.logged_out ? "false" : "true";
		reply += g_options.logged_out ? ",\"username\":\"\",\"country\":\"\""
									  : ",\"username\":\"fake_user\",\"country\":\"TR\"";
		reply += ",\"version\":" + json_quote(g_options.version) + ",\"product\":\"premium\"";
		reply += ",\"pid\":" + std::to_string(GetCurrentProcessId());
		reply += ",\"parent_pid\":" + std::to_string(g_options.parent_pid);
		reply += g_options.debug ? ",\"debug\":true}" : ",\"debug\":false}";
		write_line(pipe, reply);
		return;
	}

	if (g_options.logged_out && (op == "info" || op == "resolve" || op == "open" ||
		op == "library" || op == "search" || op == "artist" || op == "saved" || op == "save" ||
		op == "unsave" || op == "playlist_add" || op == "radio" || op == "following" ||
		op == "follow" || op == "unfollow" || op == "playlist_create" ||
		op == "playlist_rename" || op == "playlist_delete" || op == "playlist_remove" ||
		op == "liked" || op == "report_play")) {
		write_line(pipe, error_reply("not_logged_in", "not logged in"));
		return;
	}

	if (op == "config") {
		std::lock_guard<std::mutex> lock(g_cache_mutex);
		if (request.has("cache_mb")) {
			g_options.cache_mb = static_cast<int>(request.get_int64("cache_mb"));
			if (g_options.cache_mb <= 0) g_cached.clear();
		}
		if (request.get_bool("clear_cache", false)) g_cached.clear();
		if (request.has("debug")) g_options.debug = request.get_bool("debug", false);
		std::string reply = "{\"ok\":true,\"cache_mb\":" + std::to_string(g_options.cache_mb);
		reply += ",\"cache_dir\":\"C:/fake/cache\"";
		reply += ",\"cache_bytes\":" + std::to_string(static_cast<long long>(g_cached.size()) * 1000000);
		reply += g_options.debug ? ",\"debug\":true}" : ",\"debug\":false}";
		write_line(pipe, reply);
		return;
	}

	if (op == "info") {
		const flp::json::value* firstUris = request.find("uris");
		if (firstUris != nullptr && firstUris->is_array()) {
			for (const flp::json::value& item : firstUris->array) {
				if (item.is_string() && write_uri_error(pipe, item.string)) return;
			}
		}
		const int bitrate = static_cast<int>(request.get_int64("bitrate", 320));
		const flp::json::value* uris = request.find("uris");
		std::string reply = "{\"ok\":true,\"tracks\":[";
		if (uris != nullptr && uris->is_array()) {
			for (size_t i = 0; i < uris->array.size(); ++i) {
				if (i != 0) reply += ',';
				reply += make_track(uris->array[i].is_string() ? uris->array[i].string : "",
					static_cast<int>(i), bitrate);
			}
		}
		reply += "]}";
		write_line(pipe, reply);
		return;
	}

	if (op == "resolve") {
		std::string reply = "{\"ok\":true,\"tracks\":[";
		if (uri.rfind("spotify:track:", 0) == 0) {
			reply += make_track(uri, 0);
			reply += "]}";
		} else {
			int count = 3;
			std::string revision;
			{
				std::lock_guard<std::mutex> lock(g_v2.mutex);
				count = g_v2.track_count;
				revision = g_v2.revision;
			}
			for (int i = 0; i < count; ++i) {
				if (i != 0) reply += ',';
				reply += make_track("", i);
			}
			reply += "],\"context\":{";
			reply += "\"uri\":" + json_quote(uri);
			reply += ",\"name\":" + json_quote(context_name(uri));
			reply += ",\"owner\":\"Fake Owner\"";
			reply += ",\"image_url\":null";
			reply += ",\"revision\":" + json_quote(revision);
			reply += ",\"track_count\":" + std::to_string(count);
			reply += "}}";
		}
		write_line(pipe, reply);
		return;
	}

	if (op == "library") {
		write_line(pipe, make_library());
		return;
	}

	if (op == "search") {
		const std::string query = request.get_string("query");
		const int limit = static_cast<int>(request.get_int64("limit", 20));
		write_line(pipe, make_search(query, limit));
		return;
	}

	if (op == "artist") {
		write_line(pipe, make_artist(uri));
		return;
	}

	// --- protocol v3 -----------------------------------------------------------
	if (op == "saved" || op == "save" || op == "unsave") {
		const flp::json::value* uris = request.find("uris");
		if (uris == nullptr || !uris->is_array()) {
			write_line(pipe, error_reply("bad_request", "uris required"));
			return;
		}
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		std::string reply = "{\"ok\":true";
		if (op == "saved") reply += ",\"saved\":[";
		for (size_t i = 0; i < uris->array.size(); ++i) {
			const std::string track = uris->array[i].is_string() ? uris->array[i].string : "";
			if (op == "saved") {
				if (i != 0) reply += ',';
				reply += g_v2.saved.count(track) != 0 ? "true" : "false";
			} else if (op == "save") {
				g_v2.saved.insert(track);
			} else {
				g_v2.saved.erase(track);
			}
		}
		if (op == "saved") reply += "]";
		if (op != "saved") {
			// Liked Songs contents changed: bump the revision like the real bridge.
			g_v2.revision = std::to_string(std::atoi(g_v2.revision.c_str()) + 1);
		}
		reply += "}";
		write_line(pipe, reply);
		return;
	}

	if (op == "playlist_add") {
		const std::string playlist = request.get_string("playlist");
		if (playlist != "spotify:playlist:user000000000000000001") {
			write_line(pipe, error_reply("bad_request", "you cannot edit this playlist"));
			return;
		}
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		++g_v2.playlist_adds;
		write_line(pipe, "{\"ok\":true,\"revision\":\"pa" + std::to_string(g_v2.playlist_adds) + "\"}");
		return;
	}

	if (op == "radio") {
		if (uri == "spotify:track:unsupported") {
			write_line(pipe, error_reply("unsupported", "Spotify refused radio for this login"));
			return;
		}
		std::string reply = "{\"ok\":true,\"tracks\":" + make_track_ids(4);
		reply += ",\"context\":{\"uri\":" + json_quote("spotify:station:" + uri);
		reply += ",\"name\":\"Fake Track 1 Radio\",\"owner\":\"Spotify\",\"image_url\":null";
		reply += ",\"revision\":\"r1\",\"track_count\":4}}";
		write_line(pipe, reply);
		return;
	}

	// --- protocol v5: following and playlist management ------------------------
	if (op == "following" || op == "follow" || op == "unfollow") {
		const flp::json::value* uris = request.find("uris");
		if (uris == nullptr || !uris->is_array() || uris->array.empty()) {
			write_line(pipe, error_reply("bad_request", "uris required"));
			return;
		}
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		if (op == "following") {
			std::string reply = "{\"ok\":true,\"following\":[";
			for (size_t i = 0; i < uris->array.size(); ++i) {
				const std::string item = uris->array[i].is_string() ? uris->array[i].string : "";
				if (i != 0) reply += ',';
				reply += g_v2.followed.count(item) != 0 ? "true" : "false";
			}
			reply += "]}";
			write_line(pipe, reply);
			return;
		}
		for (const flp::json::value& value : uris->array) {
			const std::string item = value.is_string() ? value.string : "";
			if (item.rfind("spotify:artist:", 0) != 0 && item.rfind("spotify:album:", 0) != 0 &&
				item.rfind("spotify:playlist:", 0) != 0) {
				write_line(pipe, error_reply("bad_request", "not an artist, album or playlist"));
				return;
			}
			if (op == "follow") g_v2.followed.insert(item);
			else g_v2.followed.erase(item);
		}
		write_line(pipe, "{\"ok\":true}");
		return;
	}

	if (op == "playlist_create") {
		const std::string name = request.get_string("name");
		if (name.empty()) {
			write_line(pipe, error_reply("bad_request", "name must not be empty"));
			return;
		}
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		++g_v2.playlist_edits;
		const std::string suffix = std::to_string(g_v2.playlist_edits);
		char id[64];
		std::snprintf(id, sizeof(id), "created%017lld",
			static_cast<long long>(g_v2.playlist_edits));
		const std::string created = std::string("spotify:playlist:") + id;
		g_v2.created_playlists.emplace_back(created, name);
		g_v2.followed.insert(created);
		std::string reply = "{\"ok\":true,\"playlist\":";
		reply += make_item(created, "playlist", name, "fake_user", "pc" + suffix, 0, 1);
		reply += ",\"revision\":\"pc" + suffix + "\"}";
		write_line(pipe, reply);
		return;
	}

	if (op == "playlist_rename") {
		const std::string playlist = request.get_string("playlist");
		const std::string name = request.get_string("name");
		if (name.empty()) {
			write_line(pipe, error_reply("bad_request", "name must not be empty"));
			return;
		}
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		bool found = false;
		if (playlist == "spotify:playlist:user000000000000000001") {
			g_v2.user_playlist_name = name;
			found = true;
		} else {
			for (auto& created : g_v2.created_playlists) {
				if (created.first == playlist) {
					created.second = name;
					found = true;
				}
			}
		}
		if (!found) {
			write_line(pipe, error_reply("bad_request", "no such playlist"));
			return;
		}
		++g_v2.playlist_edits;
		write_line(pipe, "{\"ok\":true,\"revision\":\"pr" +
			std::to_string(g_v2.playlist_edits) + "\"}");
		return;
	}

	if (op == "playlist_delete") {
		const std::string playlist = request.get_string("playlist");
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		if (playlist == "spotify:playlist:user000000000000000001") {
			g_v2.user_playlist_present = false;
		} else {
			for (auto it = g_v2.created_playlists.begin(); it != g_v2.created_playlists.end(); ++it) {
				if (it->first == playlist) {
					g_v2.created_playlists.erase(it);
					break;
				}
			}
		}
		g_v2.followed.erase(playlist);
		++g_v2.playlist_edits;
		write_line(pipe, "{\"ok\":true}");
		return;
	}

	if (op == "playlist_remove") {
		const std::string playlist = request.get_string("playlist");
		const flp::json::value* items = request.find("items");
		if (playlist != "spotify:playlist:user000000000000000001" || items == nullptr ||
			!items->is_array() || items->array.empty()) {
			write_line(pipe, error_reply("bad_request", "items required for an editable playlist"));
			return;
		}
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		std::vector<int> indices;
		std::set<int> seen;
		for (const flp::json::value& value : items->array) {
			const std::string track = value.get_string("uri");
			const int index = static_cast<int>(value.get_int64("index", -1));
			if (index < 0 || index >= static_cast<int>(g_v2.user_playlist_contents.size()) ||
				g_v2.user_playlist_contents[static_cast<size_t>(index)] != track) {
				write_line(pipe, error_reply("bad_request",
					"the playlist changed; the item is not at that index"));
				return;
			}
			if (!seen.insert(index).second) {
				write_line(pipe, error_reply("bad_request", "index given twice"));
				return;
			}
			indices.push_back(index);
		}
		std::sort(indices.begin(), indices.end(), std::greater<int>());
		for (const int index : indices) {
			g_v2.user_playlist_contents.erase(g_v2.user_playlist_contents.begin() + index);
		}
		++g_v2.playlist_edits;
		write_line(pipe, "{\"ok\":true,\"revision\":\"pm" +
			std::to_string(g_v2.playlist_edits) + "\"}");
		return;
	}

	// --- protocol v6 -----------------------------------------------------------
	if (op == "shutdown") {
		// Like the real bridge: only the process named by --parent-pid may stop it.
		ULONG client = 0;
		if (g_options.parent_pid != 0 &&
			(!GetNamedPipeClientProcessId(pipe, &client) || client != g_options.parent_pid)) {
			write_line(pipe, error_reply("bad_request",
				"the bridge belongs to another process; only it can shut the bridge down"));
			return;
		}
		write_line(pipe, "{\"ok\":true}");
		FlushFileBuffers(pipe);
		log_line("shutdown by the owning process");
		std::fflush(stdout);
		ExitProcess(0);
	}

	if (op == "logout") {
		g_options.logged_out = true;
		write_line(pipe, "{\"ok\":true}");
		return;
	}

	if (op == "liked") {
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		std::string reply = "{\"ok\":true,\"uris\":[";
		bool first = true;
		for (const std::string& track : g_v2.saved) {
			if (!first) reply += ',';
			first = false;
			reply += json_quote(track);
		}
		reply += "]}";
		write_line(pipe, reply);
		return;
	}

	if (op == "report_play") {
		const long long day = 24LL * 60 * 60 * 1000;
		const long long played = request.get_int64("ms_played", -1);
		const long long duration = request.get_int64("duration_ms", 0);
		if (uri.rfind("spotify:track:", 0) != 0 || request.get_int64("started_at", 0) <= 0 ||
			played < 0 || played > day || duration < 0 || duration > day) {
			write_line(pipe, error_reply("bad_request", "report_play needs a track, started_at and ms_played"));
			return;
		}
		{
			std::lock_guard<std::mutex> lock(g_v2.mutex);
			++g_options.play_reports;
		}
		write_line(pipe, "{\"ok\":true}");
		return;
	}

	if (op == "test_control") {
		std::lock_guard<std::mutex> lock(g_v2.mutex);
		if (request.has("revision")) g_v2.revision = request.get_string("revision");
		if (request.has("tracks")) g_v2.track_count = static_cast<int>(request.get_int64("tracks"));
		if (request.has("logged_out")) g_options.logged_out = request.get_bool("logged_out", false);
		std::string reply = "{\"ok\":true,\"revision\":" + json_quote(g_v2.revision);
		reply += ",\"tracks\":" + std::to_string(g_v2.track_count);
		reply += ",\"play_reports\":" + std::to_string(g_options.play_reports) + "}";
		write_line(pipe, reply);
		return;
	}

	if (op == "open") {
		if (write_uri_error(pipe, uri)) return;
		bool fromCache = false;
		{
			std::lock_guard<std::mutex> lock(g_cache_mutex);
			if (g_options.cache_mb > 0) fromCache = !g_cached.insert(uri).second;
		}
		const int bitrate = static_cast<int>(request.get_int64("bitrate", 320));
		const long long startMs = request.get_int64("start_ms", 0);
		const long long startFrame = static_cast<long long>(
			std::llround(static_cast<double>(startMs) * g_options.rate / 1000.0));
		std::string header = "{\"ok\":true,\"track\":";
		header += make_track(uri, 0, bitrate);
		header += ",\"sample_rate\":" + std::to_string(g_options.rate);
		header += ",\"channels\":" + std::to_string(g_options.channels);
		header += ",\"start_frame\":" + std::to_string(startFrame);
		if (fromCache) header += ",\"from_cache\":true";
		header += "}";
		if (!write_line(pipe, header)) return;
		stream_pcm(pipe, startFrame);
		// Wait until the client has consumed the PCM before closing; closing a
		// pipe with unread data discards the tail.
		FlushFileBuffers(pipe);
		return;
	}

	write_line(pipe, error_reply("bad_request", "unknown op"));
}

void pump(HANDLE pipe) {
	handle_connection(pipe);
	DisconnectNamedPipe(pipe);
	CloseHandle(pipe);
}

[[noreturn]] void serve() {
	const std::wstring path = L"\\\\.\\pipe\\" + g_options.pipe;
	for (;;) {
		HANDLE pipe = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 65536,
			65536, 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE) {
			std::printf("[fake_bridge] CreateNamedPipe failed: %lu\n", GetLastError());
			Sleep(200);
			continue;
		}
		const BOOL connected = ConnectNamedPipe(pipe, nullptr);
		if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
			CloseHandle(pipe);
			continue;
		}
		std::thread(pump, pipe).detach();
	}
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	for (int i = 1; i < argc; ++i) {
		const std::wstring arg = argv[i];
		if (arg == L"--pipe" && i + 1 < argc) {
			g_options.pipe = argv[++i];
		} else if (arg == L"--duration-ms" && i + 1 < argc) {
			g_options.duration_ms = _wtoi64(argv[++i]);
		} else if (arg == L"--rate" && i + 1 < argc) {
			g_options.rate = _wtoi(argv[++i]);
		} else if (arg == L"--channels" && i + 1 < argc) {
			g_options.channels = _wtoi(argv[++i]);
		} else if (arg == L"--realtime") {
			g_options.realtime = true;
		} else if (arg == L"--logged-out") {
			g_options.logged_out = true;
		} else if (arg == L"--revision" && i + 1 < argc) {
			const std::wstring value = argv[++i];
			g_v2.revision.clear();
			for (const wchar_t c : value) {
				if (c < 128) g_v2.revision.push_back(static_cast<char>(c));
			}
		} else if (arg == L"--tracks" && i + 1 < argc) {
			g_v2.track_count = _wtoi(argv[++i]);
		} else if (arg == L"--quiet") {
			g_options.quiet = true;
		} else if (arg == L"--cache-mb" && i + 1 < argc) {
			g_options.cache_mb = _wtoi(argv[++i]);
		} else if (arg == L"--parent-pid" && i + 1 < argc) {
			g_options.parent_pid = static_cast<unsigned long>(_wtoi64(argv[++i]));
		} else if (arg == L"--version" && i + 1 < argc) {
			const std::wstring value = argv[++i];
			g_options.version.clear();
			for (const wchar_t c : value) {
				if (c < 128) g_options.version.push_back(static_cast<char>(c));
			}
		} else if (arg == L"--debug") {
			g_options.debug = true;
		}
	}

	g_v2.followed.insert("spotify:artist:art0000000000000000001");
	g_v2.followed.insert("spotify:album:alb0000000000000000001");
	g_v2.followed.insert("spotify:playlist:user000000000000000001");
	g_v2.user_playlist_contents = {
		"spotify:track:plfirst000000000000001",
		"spotify:track:plsecond00000000000001",
		"spotify:track:plthird000000000000001",
	};

	serve();
}
