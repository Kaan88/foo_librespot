// Owns the lifetime of librespot-bridge.exe (see PROTOCOL.md).
//
// The executable ships next to foo_librespot.dll. On first need we probe the
// named pipe; if it does not answer we CreateProcess it hidden with
// --parent-pid, then wait for the pipe. Thread-safe.

#pragma once

#include "protocol.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace flp {

class bridge_process {
public:
	explicit bridge_process(std::wstring pipe_name = L"foo_librespot");
	~bridge_process();
	bridge_process(const bridge_process&) = delete;
	bridge_process& operator=(const bridge_process&) = delete;

	static bridge_process& instance();

	void set_exe_path(std::wstring exe_path);
	std::wstring exe_path() const;

	// Status probe that never starts the bridge. Returns false when the pipe
	// cannot be reached; `error` then carries a human-readable reason.
	bool probe(bridge_status& out, std::string& error, unsigned timeout_ms = 800);

	// Ensures the pipe answers; starts the bridge if needed. Throws pipe_error
	// or bridge_error on failure. Waits up to start_timeout_ms for startup.
	void ensure_running(const cancel_token& ct, unsigned start_timeout_ms = 10000);

	// Launches "librespot-bridge.exe login" in a visible console window.
	bool launch_login(std::string& error);

	// Closes the handle to a process we started (does not kill it: the bridge
	// exits by itself when the parent pid goes away).
	void release();

private:
	bool try_status(bridge_status& out, std::string& error, unsigned timeout_ms);
	bool process_alive_locked();
	bool spawn_locked(std::string& error, bool new_console);

	std::wstring m_pipe_name;
	mutable std::mutex m_mutex;
	std::wstring m_exe;
	std::intptr_t m_process = 0;
};

std::string utf8_from_wide(const std::wstring& text);
std::wstring wide_from_utf8(const std::string& text);

} // namespace flp
