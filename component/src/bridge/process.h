// Owns the lifetime of librespot-bridge.exe (see PROTOCOL.md).
//
// The executable ships next to foo_librespot.dll (or in its parent folder for
// multi-architecture packages). On first need we check whether the named pipe
// accepts connections; if it does not, we CreateProcess the bridge hidden with
// --parent-pid, then wait for the pipe. Readiness is pipe connectability, never
// a status round trip: the bridge answers status immediately but may still be
// connecting to Spotify. Thread-safe.

#pragma once

#include "protocol.h"

#include <cstdint>
#include <functional>
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

	// Audio cache limit in MiB passed to a spawned bridge as --cache-mb
	// (0 = off). Negative (the default) passes no flag: the bridge's own
	// default (off) applies.
	void set_cache_mb(int cache_mb);

	// Passes --debug to a spawned bridge.
	void set_debug(bool debug);

	// The bridge version this client was built with. A bridge already on the
	// pipe that reports another version, and belongs to another live process,
	// is reported once through the warning sink.
	void set_expected_version(std::string version);

	// Receives one-line warnings about the bridge found on the pipe (another
	// instance's bridge of a different version, a leftover that does not go
	// away). Called with the internal lock held: it must not call back in.
	void set_warning_sink(std::function<void(const std::string&)> sink);

	// True when a server accepts connections on our pipe right now. This is
	// the readiness test: it never sends a request, so it cannot block behind
	// session creation.
	bool pipe_reachable(unsigned timeout_ms = 0);

	// Status probe that never starts the bridge. Returns false when the pipe
	// cannot be reached; `error` then carries a human-readable reason.
	bool probe(bridge_status& out, std::string& error, unsigned timeout_ms = 800);

	// Ensures the pipe accepts connections; starts the bridge if needed and no
	// bridge process of ours is alive. Throws pipe_error on failure. Waits up
	// to start_timeout_ms (default 15 s) for startup.
	void ensure_running(const cancel_token& ct, unsigned start_timeout_ms = 15000);

	// Launches "librespot-bridge.exe login" in a visible console window. With
	// `process`, the caller receives the login process handle and must close it
	// with close_process_handle().
	bool launch_login(std::string& error, std::intptr_t* process = nullptr);

	// True when the bridge on the pipe is one this object started (or adopted:
	// a bridge whose --parent-pid is this process) and it is still running.
	bool owns_running_bridge();

	enum class stop_result { not_ours, stopped, terminated, failed };

	// Stops the bridge this object started, and only that one: asks it with
	// the shutdown op (which the bridge accepts from its parent only), waits up
	// to wait_ms for it to exit and terminates it as a last resort. A bridge
	// another process started is never touched (not_ours).
	// `final_stop` (foobar2000 is quitting) also makes every later
	// ensure_running() throw `cancelled` instead of starting a bridge again: a
	// decoder or album art lookup still running must not respawn the bridge
	// that was just stopped.
	stop_result stop_owned(unsigned wait_ms = 3000, bool final_stop = false);

	// Closes the handle to a process we started (does not kill it: the bridge
	// exits by itself when the parent pid goes away).
	void release();

private:
	bool try_status(bridge_status& out, std::string& error, unsigned timeout_ms);
	bool process_alive_locked();
	bool pipe_reachable_locked(unsigned timeout_ms);
	bool spawn_locked(std::string& error, bool new_console, std::intptr_t* login_process);
	void check_foreign_bridge_locked(const cancel_token& ct);
	void warn_locked(const std::string& text);

	std::wstring m_pipe_name;
	mutable std::mutex m_mutex;
	std::wstring m_exe;
	std::intptr_t m_process = 0;
	unsigned long m_exit_code = 0;
	int m_cache_mb = -1;
	bool m_debug = false;
	std::string m_expected_version;
	std::function<void(const std::string&)> m_warn;
	// The bridge found on the pipe (not started by us) has been checked; reset
	// whenever the pipe is found unreachable, so a later bridge is checked too.
	bool m_foreign_checked = false;
	bool m_version_warned = false;
	bool m_final_stop = false; // set by stop_owned(final_stop): never spawn again
};

std::string utf8_from_wide(const std::wstring& text);
std::wstring wide_from_utf8(const std::string& text);

// Whether the process `pid` is still running (true when that cannot be told,
// e.g. access denied).
bool process_running(unsigned long pid);

// Waits for a process handle (from launch_login) to be signaled; returns true
// when it exited, false on timeout or cancellation (cancellation is checked
// every 200 ms; timeout_ms 0 = no limit).
bool wait_process_exit(std::intptr_t process, const cancel_token& ct, unsigned timeout_ms = 0);
void close_process_handle(std::intptr_t process);

} // namespace flp
