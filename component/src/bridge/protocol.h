// Client for proto/PROTOCOL.md (bridge protocol v1).
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
	std::string version;
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
	bridge_error(const std::string& message, const std::string& error_code)
		: std::runtime_error(message), code(error_code) {}
	std::string code;
};

struct open_result {
	track_info track;
	int sample_rate = 44100;
	int channels = 2;
	long long start_frame = 0;
	std::unique_ptr<pipe_stream> stream;
};

class bridge_client {
public:
	explicit bridge_client(std::wstring pipe_name = L"foo_librespot");

	const std::wstring& pipe_name() const { return m_pipe_name; }

	bridge_status status(const cancel_token& ct, unsigned connect_timeout_ms = 1500,
		unsigned reply_timeout_ms = 10000);

	std::vector<track_info> info(const std::vector<std::string>& uris, const cancel_token& ct,
		unsigned connect_timeout_ms = 5000, unsigned reply_timeout_ms = 30000);

	std::vector<track_info> resolve(const std::string& uri, const cancel_token& ct,
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
