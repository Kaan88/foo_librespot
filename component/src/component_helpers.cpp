#include "stdafx.h"
#include "component_helpers.h"

#include "bridge/process.h"

#include <SDK/console.h>
#include <SDK/core_api.h>

#include <mutex>

namespace flp {
namespace {

std::wstring bridge_exe_beside_component() {
	const char* dll = core_api::get_my_full_path();
	if (dll == nullptr) return std::wstring();
	std::string path(dll);
	const size_t pos = path.find_last_of("\\/");
	if (pos == std::string::npos) return wide_from_utf8("librespot-bridge.exe");
	return wide_from_utf8(path.substr(0, pos) + "\\librespot-bridge.exe");
}

} // namespace

void init_bridge_paths() {
	static std::once_flag once;
	std::call_once(once, [] { bridge_process::instance().set_exe_path(bridge_exe_beside_component()); });
}

cancel_token make_cancel(abort_callback& aborter) {
	cancel_token token;
	token.is_aborting = [&aborter] { return aborter.is_aborting(); };
	return token;
}

void ensure_bridge_started(abort_callback& aborter) {
	init_bridge_paths();
	try {
		bridge_process::instance().ensure_running(make_cancel(aborter));
	} catch (const cancelled&) {
		throw exception_aborted();
	} catch (const std::exception& error) {
		throw_io_error(error);
	}
}

void throw_io_error(const std::exception& error) {
	const auto* bridgeFailure = dynamic_cast<const bridge_error*>(&error);
	std::string detail = error.what();
	if (bridgeFailure != nullptr && !bridgeFailure->code.empty()) {
		detail += " (code: " + bridgeFailure->code + ")";
	}
	console::print("foo_librespot: ", detail.c_str());

	if (bridgeFailure != nullptr && bridgeFailure->code == "not_logged_in") {
		throw exception_io_denied(
			"Spotify: not logged in \xE2\x80\x94 Preferences > Tools > foo_librespot");
	}
	throw exception_io_denied(detail.c_str());
}

bool is_spotify_track_path(const char* path) {
	if (path == nullptr) return false;
	return strncmp(path, "spotify:track:", 14) == 0 && path[14] != '\0';
}

bool is_spotify_track_uri(const std::string& uri) { return is_spotify_track_path(uri.c_str()); }

std::string track_path(const track_info& track) {
	if (is_spotify_track_uri(track.uri)) return track.uri;
	if (track.uri.compare(0, 8, "spotify:") == 0) return track.uri;
	return std::string();
}

} // namespace flp
