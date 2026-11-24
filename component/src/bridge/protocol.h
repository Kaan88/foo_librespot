// Client for proto/PROTOCOL.md (bridge protocol v1 plus the v2 additions).
// One connection per request; `open` keeps the connection for raw PCM.

#pragma once

#include "json.h"
#include "pipe.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace flp {

struct bridge_status {
	bool logged_in = false;
	std::string username;
	std::string country;
	std::string state;      // "connecting", "connected" or "error"
	std::string last_error; // reason of the last failed session creation
	std::string version;
	// Optional (v4): the account's product type ("premium", "free", ...) and
	// the error code classifying last_error.
	std::string product;
	std::string last_error_code;
	// Optional (v6): the bridge's process id and the --parent-pid it watches
	// (0 when none or for older bridges), and whether debug logging is on.
	unsigned long pid = 0;
	unsigned long parent_pid = 0;
	bool debug = false;
};

// One play for the v6 report_play op (see PROTOCOL.md).
struct play_report {
	std::string uri;          // spotify:track:ID
	std::string context_uri;  // playlist/album the track was played from; empty = the track
	long long started_at = 0; // unix ms
	long long ms_played = 0;
	long long duration_ms = 0;
	std::string reason_start; // clickrow, trackdone, fwdbtn, backbtn, ...
	std::string reason_end;   // trackdone, endplay, fwdbtn, ...
	bool shuffle = false;
	std::string audio_format;
};

// Reply of the v4 `config` op: the audio cache as the bridge sees it.
struct config_result {
	int cache_mb = 0;          // limit in MiB, 0 = off
	std::string cache_dir;
	long long cache_bytes = 0; // size of the cached audio on disk
};

struct track_info {
	std::string uri;
	std::string title;
	std::vector<std::string> artists;
	std::string album;
	std::string album_artist;
	int track_number = 0;
	int disc_number = 0;
	std::string date;
	long long duration_ms = 0;
	std::string codec;
	int bitrate = 0;
	std::string cover_url;
	// v3, optional: spotify:album:ID and the artists' spotify:artist:ID URIs.
	std::string album_uri;
	std::vector<std::string> artist_uris;

	bool has_track_gain = false;
	bool has_track_peak = false;
	bool has_album_gain = false;
	bool has_album_peak = false;
	double track_gain_db = 0.0;
	double track_peak = 0.0;
	double album_gain_db = 0.0;
	double album_peak = 0.0;

	bool playable = true;
};

// Error reply from the bridge; `code` is one of the protocol's error codes.
struct bridge_error : std::runtime_error {
	bridge_error(const std::string& message, const std::string& error_code, int retry_seconds = 0)
		: std::runtime_error(message), code(error_code), retry_after(retry_seconds) {}
	std::string code;
	// v4, rate_limited only: advised wait in seconds (0 = unknown).
	int retry_after = 0;
};

// The context a resolve reply describes (album, playlist, Liked Songs), v2.
struct context_info {
	std::string uri;
	std::string name;
	std::string owner;
	std::string image_url;
	std::string revision; // opaque; changes whenever the contents change
	long long track_count = 0;
};

// One entry of the Spotify tab (v2 `library`, `search`, `artist`).
struct library_item {
	std::string uri;
	std::string kind; // "playlist", "album", "artist" or "collection"
	std::string name;
	std::string subtitle;
	std::string image_url;
	std::string revision;
	long long track_count = 0;
	bool editable = false; // v3: the user can add tracks to this playlist
};

struct library_section {
	std::string id;
	std::string title;
	std::vector<library_item> items;
};

// One entry of a v5 playlist_remove request: the track URI the client saw at
// that position in its copy of the playlist.
struct playlist_item_ref {
	std::string uri;
	int index = 0;
};

struct playlist_create_result {
	library_item playlist;
	std::string revision;
};

struct resolve_result {
	std::vector<track_info> tracks;
	bool has_context = false;
	context_info context;
};

struct search_result {
	std::vector<track_info> tracks;
	std::vector<library_item> albums;
	std::vector<library_item> artists;
	std::vector<library_item> playlists;
};

struct artist_result {
	library_item artist;
	std::vector<track_info> top_tracks;
	std::vector<library_item> albums;
	std::vector<library_item> singles;
};

struct open_result {
	track_info track;
	int sample_rate = 44100;
	int channels = 2;
	long long start_frame = 0;
	// v4: the encrypted audio came from the bridge's on-disk cache.
	bool from_cache = false;
	std::unique_ptr<pipe_stream> stream;
};

// Called (on the calling thread) after every successful `library` read; the
// component uses it to refresh caches derived from the library. One hook.
void set_library_read_hook(void (*hook)());

class bridge_client {
public:
	explicit bridge_client(std::wstring pipe_name = L"foo_librespot");

	const std::wstring& pipe_name() const { return m_pipe_name; }

	bridge_status status(const cancel_token& ct, unsigned connect_timeout_ms = 1500,
		unsigned reply_timeout_ms = 10000);

	std::vector<track_info> info(const std::vector<std::string>& uris, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 60000,
		int bitrate_kbps = 0);

	std::vector<track_info> resolve(const std::string& uri, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 60000);

	// v2: resolve plus the album/playlist context (name, revision, ...).
	resolve_result resolve_context(const std::string& uri, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 60000);

	// v2: contents of the Spotify tab.
	std::vector<library_section> library(const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 60000);

	// v2: search, `limit` per category.
	search_result search(const std::string& query, int limit, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 60000);

	// v2: artist page.
	artist_result artist(const std::string& uri, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 60000);

	// v3: which of `uris` are in Liked Songs (same order).
	std::vector<bool> saved(const std::vector<std::string>& uris, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);

	// v3: add to / remove from Liked Songs.
	void save(const std::vector<std::string>& uris, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);
	void unsave(const std::vector<std::string>& uris, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);

	// v3: appends tracks to one of the user's playlists; returns the new revision.
	std::string playlist_add(const std::string& playlist, const std::vector<std::string>& uris,
		const cancel_token& ct, unsigned connect_timeout_ms = 5000,
		unsigned reply_timeout_ms = 30000);

	// v5: whether artists, albums and playlists are in the user's library (same
	// order as `uris`).
	std::vector<bool> following(const std::vector<std::string>& uris, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);

	// v5: follow artists, save albums or add playlists to the library (and the
	// reverse).
	void follow(const std::vector<std::string>& uris, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);
	void unfollow(const std::vector<std::string>& uris, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);

	// v5: playlist management. Create answers the new item and revision;
	// rename and remove answer the playlist's new revision.
	playlist_create_result playlist_create(const std::string& name, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);
	std::string playlist_rename(const std::string& playlist, const std::string& name,
		const cancel_token& ct, unsigned connect_timeout_ms = 5000,
		unsigned reply_timeout_ms = 30000);
	void playlist_delete(const std::string& playlist, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);
	std::string playlist_remove(const std::string& playlist,
		const std::vector<playlist_item_ref>& items, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);

	// v3: radio based on a track, artist, album or playlist (resolve-shaped reply).
	resolve_result radio(const std::string& uri, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 60000);

	// v4: applies the audio cache size (cache_mb >= 0; negative leaves it as
	// is) and/or clears the cache, and returns the cache state. Old bridges
	// answer bad_request ("unknown op").
	config_result config(int cache_mb, bool clear_cache, const cancel_token& ct,
		unsigned connect_timeout_ms = 1500, unsigned reply_timeout_ms = 30000);

	// v6: switches debug logging in bridge.log; answers whether it is on.
	bool config_debug(bool debug, const cancel_token& ct, unsigned connect_timeout_ms = 1500,
		unsigned reply_timeout_ms = 10000);

	// v6: asks the bridge to exit. Only the process that started it (its
	// --parent-pid) may; anyone else gets bad_request.
	void shutdown(const cancel_token& ct, unsigned connect_timeout_ms = 1000,
		unsigned reply_timeout_ms = 3000);

	// v6: closes the session and deletes credentials.json.
	void logout(const cancel_token& ct, unsigned connect_timeout_ms = 1500,
		unsigned reply_timeout_ms = 15000);

	// v6: the track URIs in Liked Songs.
	std::vector<std::string> liked(const cancel_token& ct, unsigned connect_timeout_ms = 5000,
		unsigned reply_timeout_ms = 60000);

	// v6: adds a play to the account's listening history. Sent once; never
	// call this again for the same play after a failure.
	void report_play(const play_report& report, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);

	// Sends one JSON request line and returns the parsed reply. Used by the
	// console harness for the fake bridge's test-only ops; production code
	// should use the typed methods above.
	json::value raw_request(const std::string& payload, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 60000);

	// Sends `open`; on success the returned object owns the connection and the
	// caller reads interleaved float32 LE frames from it.
	open_result open(const std::string& uri, long long start_ms, int bitrate,
		const cancel_token& ct, unsigned connect_timeout_ms = 5000,
		unsigned reply_timeout_ms = 30000);

private:
	json::value request(const std::string& payload, const cancel_token& ct,
		unsigned connect_timeout_ms, unsigned reply_timeout_ms);
	std::wstring m_pipe_name;
};

} // namespace flp
