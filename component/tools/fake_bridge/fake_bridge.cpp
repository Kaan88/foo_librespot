// Fake librespot bridge used by the automated tests.
//
// Speaks proto/PROTOCOL.md v1 on a named pipe, generates a 440 Hz sine wave and
// accepts the same command line as the real helper (--parent-pid/--pipe), so it
// can be copied to a temp folder as librespot-bridge.exe and exercised through
// bridge_process (process spawn + pipe wait).

#include "../src/bridge/json.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
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
} g_options;

void log_line(const char* text) {
	if (!g_options.quiet) std::printf("[fake_bridge] %s\n", text);
}

std::string json_quote(const std::string& value) { return flp::json::quote(value); }

std::string make_track(const std::string& uri, int index = 0) {
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
	out += ",\"album_artist\":\"Fake Artist\"";
	out += ",\"track_number\":" + std::to_string(index + 1);
	out += ",\"disc_number\":1";
	out += ",\"date\":\"2026\"";
	out += ",\"duration_ms\":" + std::to_string(g_options.duration_ms);
	out += ",\"codec\":\"vorbis\"";
	out += ",\"bitrate\":320";
	out += ",\"cover_url\":\"\"";
	out += ",\"track_gain_db\":-7.25";
	out += ",\"track_peak\":0.9";
	out += ",\"album_gain_db\":null";
	out += ",\"album_peak\":null";
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

std::string error_reply(const char* code, const char* message) {
	return std::string("{\"ok\":false,\"error\":") + json_quote(message) + ",\"code\":" +
		json_quote(code) + "}";
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
		reply += ",\"version\":\"0.1.0-fake\"}";
		write_line(pipe, reply);
		return;
	}

	if (g_options.logged_out && (op == "info" || op == "resolve" || op == "open")) {
		write_line(pipe, error_reply("not_logged_in", "not logged in"));
		return;
	}

	if (op == "info") {
		const flp::json::value* uris = request.find("uris");
		std::string reply = "{\"ok\":true,\"tracks\":[";
		if (uris != nullptr && uris->is_array()) {
			for (size_t i = 0; i < uris->array.size(); ++i) {
				if (i != 0) reply += ',';
				reply += make_track(uris->array[i].is_string() ? uris->array[i].string : "",
					static_cast<int>(i));
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
		} else {
			for (int i = 0; i < 3; ++i) {
				if (i != 0) reply += ',';
				reply += make_track("", i);
			}
		}
		reply += "]}";
		write_line(pipe, reply);
		return;
	}

	if (op == "open") {
		const long long startMs = request.get_int64("start_ms", 0);
		const long long startFrame = static_cast<long long>(
			std::llround(static_cast<double>(startMs) * g_options.rate / 1000.0));
		std::string header = "{\"ok\":true,\"track\":";
		header += make_track(uri, 0);
		header += ",\"sample_rate\":" + std::to_string(g_options.rate);
		header += ",\"channels\":" + std::to_string(g_options.channels);
		header += ",\"start_frame\":" + std::to_string(startFrame);
		header += "}";
		if (!write_line(pipe, header)) return;
		stream_pcm(pipe, startFrame);
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
		} else if (arg == L"--quiet") {
			g_options.quiet = true;
		} else if (arg == L"--parent-pid" && i + 1 < argc) {
			++i; // accepted and ignored
		}
	}

	serve();
}
