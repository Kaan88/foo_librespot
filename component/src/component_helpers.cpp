#include "stdafx.h"
#include "component_helpers.h"

#include "bridge/process.h"
#include "config.h"
#include "error_policy.h"

#include <SDK/console.h>
#include <SDK/core_api.h>
#include <SDK/popup_message.h>

#include <filesystem>
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
	std::call_once(once, [] {
		bridge_process::instance().set_exe_path(bridge_exe_near_component());
		// A bridge spawned from now on starts with the configured audio cache.
		bridge_process::instance().set_cache_mb(effective_cache_mb());
	});
}

bool apply_cache_setting(int cache_mb, bool clear_cache, config_result& out, std::string& error) {
	init_bridge_paths();
	if (cache_mb >= 0) bridge_process::instance().set_cache_mb(cache_mb);
	if (!bridge_process::instance().pipe_reachable(0)) {
		// No bridge running: the next spawn gets --cache-mb, so only the
		// clearing is left, which works on the folder directly.
		return false;
	}
	try {
		bridge_client client;
		out = client.config(cache_mb, clear_cache, cancel_token(), 1500, 30000);
		return true;
	} catch (const std::exception& failure) {
		// An older bridge does not know the op; it keeps its old setting until
		// it restarts.
		error = failure.what();
		return false;
	}
}

std::string default_cache_dir() {
	wchar_t buffer[MAX_PATH * 2] = {};
	const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
		static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
	if (length == 0 || length >= sizeof(buffer) / sizeof(buffer[0])) return std::string();
	return utf8_from_wide(std::wstring(buffer, length) + L"\\foo_librespot\\cache");
}

long long cache_folder_bytes(const std::string& cache_dir) {
	if (cache_dir.empty()) return 0;
	namespace fs = std::filesystem;
	std::error_code ec;
	long long total = 0;
	fs::recursive_directory_iterator it(fs::path(wide_from_utf8(cache_dir)) / L"audio",
		fs::directory_options::skip_permission_denied, ec);
	for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		std::error_code fileError;
		if (it->is_regular_file(fileError)) {
			const auto size = it->file_size(fileError);
			if (!fileError) total += static_cast<long long>(size);
		}
	}
	return total;
}

bool clear_cache_folder(const std::string& cache_dir, std::string& error) {
	if (cache_dir.empty()) return true;
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path audio = fs::path(wide_from_utf8(cache_dir)) / L"audio";
	if (!fs::exists(audio, ec)) return true;
	fs::remove_all(audio, ec);
	if (ec) {
		error = ec.message();
		return false;
	}
	return true;
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
	return describe_error(error).message;
}

namespace {

void print_error(const std::exception& error, const char* track) {
	const auto* bridgeFailure = dynamic_cast<const bridge_error*>(&error);
	const std::string shown = describe_error(error).message;
	pfc::string_formatter text;
	text << "foo_librespot: ";
	if (track != nullptr) text << track << ": ";
	text << shown.c_str();
	if (shown.find(error.what()) == std::string::npos) text << " [" << error.what() << "]";
	if (bridgeFailure != nullptr && !bridgeFailure->code.empty()) {
		text << " (code: " << bridgeFailure->code.c_str() << ")";
	}
	console::print(text);
}

[[noreturn]] void throw_for(const error_info& info) {
	// A track that cannot be played gets the exception foobar2000 handles as a
	// per-item failure ("Object not found"), so playback can go on with the next
	// item; everything else (login, Premium, rate limit, connection) is not
	// about this one track and is reported as a denied access.
	if (info.skip_track) throw exception_io_not_found(info.message.c_str());
	throw exception_io_denied(info.message.c_str());
}

} // namespace

void throw_io_error(const std::exception& error) {
	print_error(error, nullptr);
	throw_for(describe_error(error));
}

void throw_track_error(const std::exception& error, const char* track) {
	print_error(error, track);
	const error_info info = describe_error(error);
	// One popup per kind and session: a playlist of unavailable tracks must
	// not raise one dialog per track. The console line above is per track.
	if (info.popup && first_report_of_kind(info.kind)) {
		const std::string message = info.message;
		fb2k::inMainThread([message] {
			popup_message::g_show(message.c_str(), "foo_librespot", popup_message::icon_error);
		});
	}
	throw_for(info);
}

void throw_unplayable(const char* track) {
	const bridge_error error("This track is no longer available on Spotify", "track_unavailable");
	throw_track_error(error, track);
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
