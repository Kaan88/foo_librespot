#include "stdafx.h"
#include "component_helpers.h"

#include "bridge/process.h"
#include "config.h"
#include "error_policy.h"
#include "liked_cache.h"
#include "version.h"

#include <SDK/console.h>
#include <SDK/core_api.h>
#include <SDK/initquit.h>
#include <SDK/popup_message.h>

#include <atomic>
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

// Set in initquit::on_quit. A plain atomic, so it needs no SDK service and is
// safe during DLL static initialisation.
std::atomic<bool> g_quitting{false};

// The debug setting last sent to (or spawned with) our bridge; -1 = none yet.
std::atomic<int> g_debug_applied{-1};

// Applies the Advanced Preferences debug toggle: a bridge spawned from now on
// gets --debug, and a running bridge of ours is switched live when the
// setting changed since it was last applied. Worker threads only.
void sync_debug_setting() {
	const bool wanted = cfg_debug_log.get();
	bridge_process& process = bridge_process::instance();
	process.set_debug(wanted);
	const int previous = g_debug_applied.exchange(wanted ? 1 : 0);
	if (previous == -1 || previous == (wanted ? 1 : 0)) return;
	if (!process.owns_running_bridge()) return; // another instance's: leave it alone
	try {
		bridge_client().config_debug(wanted, app_cancel_token(), 1000, 5000);
		console::print(wanted ? "foo_librespot: bridge debug logging on"
							  : "foo_librespot: bridge debug logging off");
	} catch (const std::exception& error) {
		console::print("foo_librespot: could not switch bridge debug logging: ", error.what());
	}
}

class quit_handler : public initquit {
public:
	void on_init() override {}
	void on_quit() override {
		g_quitting = true;
		// Stop the bridge this instance started, so the exe is not left running
		// (and locked) after foobar2000 exits. A bridge another instance owns is
		// never touched; the bridge itself also refuses a shutdown from anyone
		// but its parent.
		switch (bridge_process::instance().stop_owned(3000, true)) {
		case bridge_process::stop_result::not_ours:
			break;
		case bridge_process::stop_result::stopped:
			console::print("foo_librespot: bridge stopped");
			break;
		case bridge_process::stop_result::terminated:
			console::print("foo_librespot: bridge did not stop in time and was terminated");
			break;
		case bridge_process::stop_result::failed:
			console::print("foo_librespot: could not stop the bridge");
			break;
		}
	}
};

initquit_factory_t<quit_handler> g_quit_handler;

} // namespace

bool app_is_quitting() { return g_quitting.load(); }

cancel_token app_cancel_token() {
	cancel_token token;
	token.is_aborting = [] { return g_quitting.load(); };
	return token;
}

void init_bridge_paths() {
	static std::once_flag once;
	std::call_once(once, [] {
		bridge_process& process = bridge_process::instance();
		process.set_exe_path(bridge_exe_near_component());
		// A bridge spawned from now on starts with the configured audio cache.
		process.set_cache_mb(effective_cache_mb());
		process.set_debug(cfg_debug_log.get());
		process.set_expected_version(FLP_VERSION);
		process.set_warning_sink([](const std::string& text) {
			console::print("foo_librespot: ", text.c_str());
		});
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

bool log_out_spotify(std::string& error) {
	bool done = false;
	try {
		init_bridge_paths();
		if (bridge_process::instance().pipe_reachable(0)) {
			bridge_client().logout(app_cancel_token());
			done = true;
		}
	} catch (const bridge_error& failure) {
		// An older bridge has no logout op: fall back to deleting the file.
		const bool unknown_op = failure.code == "bad_request" &&
			std::string(failure.what()).find("unknown op") != std::string::npos;
		if (failure.code != "unsupported" && !unknown_op) {
			error = user_error_message(failure);
			return false;
		}
	} catch (const cancelled&) {
		// foobar2000 is quitting; `cancelled` is not a std::exception and must
		// not leave the worker thread.
		error = "cancelled";
		return false;
	} catch (const std::exception& failure) {
		error = user_error_message(failure);
		return false;
	}
	if (!done) {
		wchar_t buffer[MAX_PATH * 2] = {};
		const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer,
			static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
		if (length == 0 || length >= sizeof(buffer) / sizeof(buffer[0])) {
			error = "%APPDATA% is not set";
			return false;
		}
		const std::filesystem::path file =
			std::filesystem::path(std::wstring(buffer, length)) / L"foo_librespot" / L"credentials.json";
		std::error_code ec;
		std::filesystem::remove(file, ec);
		if (ec) {
			error = "Could not delete the stored login: " + ec.message();
			return false;
		}
	}
	liked_cache_clear();
	return true;
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
		bridge_process::instance().set_debug(cfg_debug_log.get());
		bridge_process::instance().ensure_running(make_cancel(aborter));
		sync_debug_setting();
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
