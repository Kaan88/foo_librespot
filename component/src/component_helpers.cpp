#include "stdafx.h"
#include "component_helpers.h"

#include "bridge/process.h"

#include <SDK/console.h>
#include <SDK/core_api.h>

#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace flp {
namespace {

bool file_exists(const std::wstring& path) {
	const DWORD attrs = GetFileAttributesW(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Locates librespot-bridge.exe: next to the component DLL, then in the folder
// above it. The x86 package has both at the root; the x64 package keeps them
// under x64/, next to the x64 DLL, so "next to the DLL" covers both, and the
// parent folder covers a DLL copied out of its architecture folder.
std::wstring bridge_exe_near_component() {
	const char* dll = core_api::get_my_full_path();
	if (dll == nullptr) return L"librespot-bridge.exe";

	std::wstring directory = wide_from_utf8(dll);
	const size_t slash = directory.find_last_of(L"\\/");
	if (slash == std::wstring::npos) return L"librespot-bridge.exe";
	directory.resize(slash);

	const std::wstring beside = directory + L"\\librespot-bridge.exe";
	if (file_exists(beside)) return beside;

	const size_t parentSlash = directory.find_last_of(L"\\/");
	if (parentSlash != std::wstring::npos) {
		const std::wstring parent = directory.substr(0, parentSlash) + L"\\librespot-bridge.exe";
		if (file_exists(parent)) return parent;
	}

	// Nothing found: name the place we expect, so the error says where to put it.
	return beside;
}

} // namespace

void init_bridge_paths() {
	static std::once_flag once;
	std::call_once(once, [] { bridge_process::instance().set_exe_path(bridge_exe_near_component()); });
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

std::string user_error_message(const std::exception& error) {
	const auto* failure = dynamic_cast<const bridge_error*>(&error);
	if (failure != nullptr && failure->code == "not_logged_in") {
		return "Spotify: not logged in \xE2\x80\x94 Preferences > Tools > foo_librespot";
	}

	const std::string detail = error.what();
	// The bridge reports a failed session as "connecting to Spotify failed:
	// <reason>"; show it the way foobar2000 users expect to read it.
	const std::string connectPrefix = "connecting to Spotify failed: ";
	if (detail.compare(0, connectPrefix.size(), connectPrefix) == 0) {
		return "Spotify: connecting failed \xE2\x80\x94 " + detail.substr(connectPrefix.size());
	}
	return "Spotify: " + detail;
}

void throw_io_error(const std::exception& error) {
	const auto* bridgeFailure = dynamic_cast<const bridge_error*>(&error);
	if (bridgeFailure != nullptr && !bridgeFailure->code.empty()) {
		console::print("foo_librespot: ", error.what(), " (code: ", bridgeFailure->code.c_str(), ")");
	} else {
		console::print("foo_librespot: ", error.what());
	}

	throw exception_io_denied(user_error_message(error).c_str());
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
