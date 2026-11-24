#include "protocol.h"

#include <atomic>
#include <cmath>

namespace flp {
namespace {

[[noreturn]] void raise_error(const json::value& reply) {
	const std::string message = reply.get_string("error", "unknown librespot bridge error");
	const std::string code = reply.get_string("code");
	throw bridge_error(message, code, static_cast<int>(reply.get_int64("retry_after", 0)));
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
	out.album_uri = t.get_string("album_uri");
	if (const json::value* uris = t.find("artist_uris"); uris != nullptr && uris->is_array()) {
		for (const json::value& a : uris->array) {
			if (a.is_string() && !a.string.empty()) out.artist_uris.push_back(a.string);
		}
	}
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

std::vector<track_info> parse_tracks(const json::value& reply, const char* key) {
	std::vector<track_info> out;
	const json::value* tracks = reply.find(key);
	if (tracks != nullptr && tracks->is_array()) {
		out.reserve(tracks->array.size());
		for (const json::value& t : tracks->array) out.push_back(parse_track(t));
	}
	return out;
}

std::vector<track_info> parse_tracks(const json::value& reply) {
	return parse_tracks(reply, "tracks");
}

library_item parse_item(const json::value& value) {
	library_item out;
	if (!value.is_object()) return out;
	out.uri = value.get_string("uri");
	out.kind = value.get_string("kind");
	out.name = value.get_string("name");
	out.subtitle = value.get_string("subtitle");
	out.image_url = value.get_string("image_url");
	out.revision = value.get_string("revision");
	out.track_count = value.get_int64("track_count");
	out.editable = value.get_bool("editable", false);
	return out;
}

std::vector<library_item> parse_items(const json::value& reply, const char* key) {
	std::vector<library_item> out;
	const json::value* items = reply.find(key);
	if (items != nullptr && items->is_array()) {
		out.reserve(items->array.size());
		for (const json::value& item : items->array) out.push_back(parse_item(item));
	}
	return out;
}

context_info parse_context(const json::value& value) {
	context_info out;
	if (!value.is_object()) return out;
	out.uri = value.get_string("uri");
	out.name = value.get_string("name");
	out.owner = value.get_string("owner");
	out.image_url = value.get_string("image_url");
	out.revision = value.get_string("revision");
	out.track_count = value.get_int64("track_count");
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

std::string playlist_items_json(const std::vector<playlist_item_ref>& items) {
	std::string out = "[";
	for (size_t i = 0; i < items.size(); ++i) {
		if (i != 0) out += ",";
		out += "{\"uri\":" + json::quote(items[i].uri);
		out += ",\"index\":" + std::to_string(items[i].index);
		out += "}";
	}
	out += "]";
	return out;
}

std::atomic<void (*)()>& library_read_hook() {
	static std::atomic<void (*)()> hook{nullptr};
	return hook;
}

} // namespace

void set_library_read_hook(void (*hook)()) { library_read_hook() = hook; }

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
	out.state = reply.get_string("state");
	out.last_error = reply.get_string("last_error");
	out.version = reply.get_string("version");
	out.product = reply.get_string("product");
	out.last_error_code = reply.get_string("last_error_code");
	const long long pid = reply.get_int64("pid", 0);
	const long long parent = reply.get_int64("parent_pid", 0);
	out.pid = pid > 0 ? static_cast<unsigned long>(pid) : 0;
	out.parent_pid = parent > 0 ? static_cast<unsigned long>(parent) : 0;
	out.debug = reply.get_bool("debug", false);
	return out;
}

std::vector<track_info> bridge_client::info(const std::vector<std::string>& uris,
	const cancel_token& ct, unsigned connect_timeout_ms, unsigned reply_timeout_ms,
	int bitrate_kbps) {
	if (uris.empty()) return {};
	std::string payload = "{\"op\":\"info\",\"uris\":" + uris_json(uris);
	if (bitrate_kbps > 0) payload += ",\"bitrate\":" + std::to_string(bitrate_kbps);
	payload += "}";
	const json::value reply = request(payload, ct, connect_timeout_ms, reply_timeout_ms);
	return parse_tracks(reply);
}

std::vector<track_info> bridge_client::resolve(const std::string& uri, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	return resolve_context(uri, ct, connect_timeout_ms, reply_timeout_ms).tracks;
}

resolve_result bridge_client::resolve_context(const std::string& uri, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request(
		"{\"op\":\"resolve\",\"uri\":" + json::quote(uri) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
	resolve_result out;
	out.tracks = parse_tracks(reply);
	if (const json::value* context = reply.find("context");
		context != nullptr && context->is_object()) {
		out.context = parse_context(*context);
		out.has_context = true;
	}
	return out;
}

std::vector<library_section> bridge_client::library(const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request("{\"op\":\"library\"}", ct, connect_timeout_ms,
		reply_timeout_ms);
	std::vector<library_section> out;
	const json::value* sections = reply.find("sections");
	if (sections == nullptr || !sections->is_array()) return out;
	out.reserve(sections->array.size());
	for (const json::value& section : sections->array) {
		if (!section.is_object()) continue;
		library_section parsed;
		parsed.id = section.get_string("id");
		parsed.title = section.get_string("title");
		parsed.items = parse_items(section, "items");
		out.push_back(std::move(parsed));
	}
	if (void (*hook)() = library_read_hook().load()) hook();
	return out;
}

search_result bridge_client::search(const std::string& query, int limit, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	std::string payload = "{\"op\":\"search\",\"query\":" + json::quote(query);
	if (limit > 0) payload += ",\"limit\":" + std::to_string(limit);
	payload += "}";
	const json::value reply = request(payload, ct, connect_timeout_ms, reply_timeout_ms);
	search_result out;
	out.tracks = parse_tracks(reply);
	out.albums = parse_items(reply, "albums");
	out.artists = parse_items(reply, "artists");
	out.playlists = parse_items(reply, "playlists");
	return out;
}

artist_result bridge_client::artist(const std::string& uri, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request(
		"{\"op\":\"artist\",\"uri\":" + json::quote(uri) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
	artist_result out;
	if (const json::value* artist = reply.find("artist"); artist != nullptr) {
		out.artist = parse_item(*artist);
	}
	out.top_tracks = parse_tracks(reply, "top_tracks");
	out.albums = parse_items(reply, "albums");
	out.singles = parse_items(reply, "singles");
	return out;
}

std::vector<bool> bridge_client::saved(const std::vector<std::string>& uris,
	const cancel_token& ct, unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	if (uris.empty()) return {};
	const json::value reply = request("{\"op\":\"saved\",\"uris\":" + uris_json(uris) + "}", ct,
		connect_timeout_ms, reply_timeout_ms);
	std::vector<bool> out(uris.size(), false);
	if (const json::value* flags = reply.find("saved"); flags != nullptr && flags->is_array()) {
		for (size_t i = 0; i < flags->array.size() && i < out.size(); ++i) {
			out[i] = flags->array[i].is_bool() && flags->array[i].boolean;
		}
	}
	return out;
}

void bridge_client::save(const std::vector<std::string>& uris, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	if (uris.empty()) return;
	request("{\"op\":\"save\",\"uris\":" + uris_json(uris) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
}

void bridge_client::unsave(const std::vector<std::string>& uris, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	if (uris.empty()) return;
	request("{\"op\":\"unsave\",\"uris\":" + uris_json(uris) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
}

std::string bridge_client::playlist_add(const std::string& playlist,
	const std::vector<std::string>& uris, const cancel_token& ct, unsigned connect_timeout_ms,
	unsigned reply_timeout_ms) {
	const json::value reply = request("{\"op\":\"playlist_add\",\"playlist\":" +
		json::quote(playlist) + ",\"uris\":" + uris_json(uris) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
	return reply.get_string("revision");
}

std::vector<bool> bridge_client::following(const std::vector<std::string>& uris,
	const cancel_token& ct, unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	if (uris.empty()) return {};
	const json::value reply = request("{\"op\":\"following\",\"uris\":" + uris_json(uris) + "}",
		ct, connect_timeout_ms, reply_timeout_ms);
	std::vector<bool> out(uris.size(), false);
	if (const json::value* flags = reply.find("following"); flags != nullptr && flags->is_array()) {
		for (size_t i = 0; i < flags->array.size() && i < out.size(); ++i) {
			out[i] = flags->array[i].is_bool() && flags->array[i].boolean;
		}
	}
	return out;
}

void bridge_client::follow(const std::vector<std::string>& uris, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	if (uris.empty()) return;
	request("{\"op\":\"follow\",\"uris\":" + uris_json(uris) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
}

void bridge_client::unfollow(const std::vector<std::string>& uris, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	if (uris.empty()) return;
	request("{\"op\":\"unfollow\",\"uris\":" + uris_json(uris) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
}

playlist_create_result bridge_client::playlist_create(const std::string& name,
	const cancel_token& ct, unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request(
		"{\"op\":\"playlist_create\",\"name\":" + json::quote(name) + "}", ct,
		connect_timeout_ms, reply_timeout_ms);
	playlist_create_result out;
	if (const json::value* playlist = reply.find("playlist"); playlist != nullptr) {
		out.playlist = parse_item(*playlist);
	}
	out.revision = reply.get_string("revision");
	return out;
}

std::string bridge_client::playlist_rename(const std::string& playlist, const std::string& name,
	const cancel_token& ct, unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request("{\"op\":\"playlist_rename\",\"playlist\":" +
		json::quote(playlist) + ",\"name\":" + json::quote(name) + "}", ct, connect_timeout_ms,
		reply_timeout_ms);
	return reply.get_string("revision");
}

void bridge_client::playlist_delete(const std::string& playlist, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	request("{\"op\":\"playlist_delete\",\"playlist\":" + json::quote(playlist) + "}", ct,
		connect_timeout_ms, reply_timeout_ms);
}

std::string bridge_client::playlist_remove(const std::string& playlist,
	const std::vector<playlist_item_ref>& items, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request("{\"op\":\"playlist_remove\",\"playlist\":" +
		json::quote(playlist) + ",\"items\":" + playlist_items_json(items) + "}", ct,
		connect_timeout_ms, reply_timeout_ms);
	return reply.get_string("revision");
}

resolve_result bridge_client::radio(const std::string& uri, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request("{\"op\":\"radio\",\"uri\":" + json::quote(uri) + "}", ct,
		connect_timeout_ms, reply_timeout_ms);
	resolve_result out;
	out.tracks = parse_tracks(reply);
	if (const json::value* context = reply.find("context");
		context != nullptr && context->is_object()) {
		out.context = parse_context(*context);
		out.has_context = true;
	}
	return out;
}

config_result bridge_client::config(int cache_mb, bool clear_cache, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	std::string payload = "{\"op\":\"config\"";
	if (cache_mb >= 0) payload += ",\"cache_mb\":" + std::to_string(cache_mb);
	if (clear_cache) payload += ",\"clear_cache\":true";
	payload += "}";
	const json::value reply = request(payload, ct, connect_timeout_ms, reply_timeout_ms);
	config_result out;
	out.cache_mb = static_cast<int>(reply.get_int64("cache_mb", 0));
	out.cache_dir = reply.get_string("cache_dir");
	out.cache_bytes = reply.get_int64("cache_bytes", 0);
	return out;
}

bool bridge_client::config_debug(bool debug, const cancel_token& ct, unsigned connect_timeout_ms,
	unsigned reply_timeout_ms) {
	const json::value reply = request(std::string("{\"op\":\"config\",\"debug\":") +
		(debug ? "true" : "false") + "}", ct, connect_timeout_ms, reply_timeout_ms);
	return reply.get_bool("debug", false);
}

void bridge_client::shutdown(const cancel_token& ct, unsigned connect_timeout_ms,
	unsigned reply_timeout_ms) {
	request("{\"op\":\"shutdown\"}", ct, connect_timeout_ms, reply_timeout_ms);
}

void bridge_client::logout(const cancel_token& ct, unsigned connect_timeout_ms,
	unsigned reply_timeout_ms) {
	request("{\"op\":\"logout\"}", ct, connect_timeout_ms, reply_timeout_ms);
}

std::vector<std::string> bridge_client::liked(const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	const json::value reply = request("{\"op\":\"liked\"}", ct, connect_timeout_ms,
		reply_timeout_ms);
	std::vector<std::string> out;
	if (const json::value* uris = reply.find("uris"); uris != nullptr && uris->is_array()) {
		out.reserve(uris->array.size());
		for (const json::value& uri : uris->array) {
			if (uri.is_string() && !uri.string.empty()) out.push_back(uri.string);
		}
	}
	return out;
}

void bridge_client::report_play(const play_report& r, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	std::string payload = "{\"op\":\"report_play\",\"uri\":" + json::quote(r.uri);
	if (!r.context_uri.empty()) payload += ",\"context_uri\":" + json::quote(r.context_uri);
	payload += ",\"started_at\":" + std::to_string(r.started_at);
	payload += ",\"ms_played\":" + std::to_string(r.ms_played);
	payload += ",\"duration_ms\":" + std::to_string(r.duration_ms);
	if (!r.reason_start.empty()) payload += ",\"reason_start\":" + json::quote(r.reason_start);
	if (!r.reason_end.empty()) payload += ",\"reason_end\":" + json::quote(r.reason_end);
	if (r.shuffle) payload += ",\"shuffle\":true";
	if (!r.audio_format.empty()) payload += ",\"audio_format\":" + json::quote(r.audio_format);
	payload += "}";
	request(payload, ct, connect_timeout_ms, reply_timeout_ms);
}

json::value bridge_client::raw_request(const std::string& payload, const cancel_token& ct,
	unsigned connect_timeout_ms, unsigned reply_timeout_ms) {
	return request(payload, ct, connect_timeout_ms, reply_timeout_ms);
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
	out.from_cache = header.get_bool("from_cache", false);
	out.stream = std::move(pipe);
	return out;
}

} // namespace flp
