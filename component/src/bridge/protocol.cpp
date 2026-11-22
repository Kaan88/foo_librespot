#include "protocol.h"

#include <cmath>

namespace flp {
namespace {

[[noreturn]] void raise_error(const json::value& reply) {
	const std::string message = reply.get_string("error", "unknown librespot bridge error");
	const std::string code = reply.get_string("code");
	throw bridge_error(message, code);
}

void require_ok(const json::value& reply) {
	if (!reply.is_object()) throw bridge_error("malformed reply from the librespot bridge", "internal");
	if (!reply.get_bool("ok", false)) raise_error(reply);
}

track_info parse_track(const json::value& t) {
	track_info out;
	if (!t.is_object()) return out;

	out.uri = t.get_string("uri");
	out.title = t.get_string("title");
	if (const json::value* artists = t.find("artists"); artists != nullptr && artists->is_array()) {
		for (const json::value& a : artists->array) {
			if (a.is_string()) out.artists.push_back(a.string);
		}
	}
	out.album = t.get_string("album");
	out.album_artist = t.get_string("album_artist");
	out.track_number = static_cast<int>(t.get_int64("track_number"));
	out.disc_number = static_cast<int>(t.get_int64("disc_number"));
	out.date = t.get_string("date");
	out.duration_ms = t.get_int64("duration_ms");
	out.codec = t.get_string("codec");
	out.bitrate = static_cast<int>(t.get_int64("bitrate"));
	out.cover_url = t.get_string("cover_url");
	out.playable = t.get_bool("playable", true);

	if (const json::value* v = t.find("track_gain_db"); v != nullptr && v->is_number()) {
		out.track_gain_db = v->number;
		out.has_track_gain = true;
	}
	if (const json::value* v = t.find("track_peak"); v != nullptr && v->is_number()) {
		out.track_peak = v->number;
		out.has_track_peak = true;
	}
	if (const json::value* v = t.find("album_gain_db"); v != nullptr && v->is_number()) {
		out.album_gain_db = v->number;
		out.has_album_gain = true;
	}
	if (const json::value* v = t.find("album_peak"); v != nullptr && v->is_number()) {
		out.album_peak = v->number;
		out.has_album_peak = true;
	}
	return out;
}

std::vector<track_info> parse_tracks(const json::value& reply) {
	std::vector<track_info> out;
	const json::value* tracks = reply.find("tracks");
	if (tracks != nullptr && tracks->is_array()) {
		out.reserve(tracks->array.size());
		for (const json::value& t : tracks->array) out.push_back(parse_track(t));
	}
	return out;
}

std::string uris_json(const std::vector<std::string>& uris) {
	std::string out = "[";
	for (size_t i = 0; i < uris.size(); ++i) {
		if (i != 0) out += ",";
		out += json::quote(uris[i]);
	}
	out += "]";
	return out;
}

} // namespace

bridge_client::bridge_client(std::wstring pipe_name) : m_pipe_name(std::move(pipe_name)) {}

json::value bridge_client::request(const std::string& payload, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	pipe_stream pipe;
	pipe.connect(m_pipe_name, connect_timeout_ms, ct);

	std::string line = payload;
	line += '\n';
	pipe.write_all(line.data(), line.size(), ct);

	const std::string replyLine = pipe.read_line(ct, reply_timeout_ms);
	json::value reply;
	std::string error;
	if (!json::parse(replyLine, reply, error)) {
		throw bridge_error("malformed reply from the librespot bridge: " + error, "internal");
	}
	require_ok(reply);
	return reply;
}

bridge_status bridge_client::status(const cancel_token& ct, unsigned connect_timeout_ms,
	unsigned reply_timeout_ms) {
	const json::value reply = request("{\"op\":\"status\"}", ct, connect_timeout_ms, reply_timeout_ms);
	bridge_status out;
	out.logged_in = reply.get_bool("logged_in", false);
	out.username = reply.get_string("username");
	out.country = reply.get_string("country");
	out.version = reply.get_string("version");
	return out;
}

std::vector<track_info> bridge_client::info(const std::vector<std::string>& uris,
	const cancel_token& ct, unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	if (uris.empty()) return {};
	const json::value reply = request(
		"{\"op\":\"info\",\"uris\":" + uris_json(uris) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
	return parse_tracks(reply);
}

std::vector<track_info> bridge_client::resolve(const std::string& uri, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request(
		"{\"op\":\"resolve\",\"uri\":" + json::quote(uri) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
	return parse_tracks(reply);
}

open_result bridge_client::open(const std::string& uri, long long start_ms, int bitrate,
	const cancel_token& ct, unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	auto pipe = std::make_unique<pipe_stream>();
	pipe->connect(m_pipe_name, connect_timeout_ms, ct);

	const std::string line = "{\"op\":\"open\",\"uri\":" + json::quote(uri) +
		",\"start_ms\":" + std::to_string(start_ms) + ",\"bitrate\":" + std::to_string(bitrate) +
		"}\n";
	pipe->write_all(line.data(), line.size(), ct);

	const std::string headerLine = pipe->read_line(ct, reply_timeout_ms);
	json::value header;
	std::string error;
	if (!json::parse(headerLine, header, error)) {
		throw bridge_error("malformed reply to open: " + error, "internal");
	}
	require_ok(header);

	open_result out;
	if (const json::value* track = header.find("track"); track != nullptr) {
		out.track = parse_track(*track);
	}
	out.sample_rate = static_cast<int>(header.get_int64("sample_rate", 44100));
	out.channels = static_cast<int>(header.get_int64("channels", 2));
	out.start_frame = header.get_int64("start_frame", 0);
	out.stream = std::move(pipe);
	return out;
}

} // namespace flp
