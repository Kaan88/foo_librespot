#include "process.h"

#include <algorithm>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace flp {
namespace {

std::string win32_message(DWORD code) {
	char* buffer = nullptr;
	const DWORD n = FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<char*>(&buffer),
		0, nullptr);
	std::string out;
	if (n != 0 && buffer != nullptr) {
		out.assign(buffer, n);
		while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) {
			out.pop_back();
		}
	}
	if (buffer != nullptr) LocalFree(buffer);
	if (out.empty()) out = "Win32 error " + std::to_string(static_cast<unsigned long>(code));
	return out;
}

std::wstring quote_arg(const std::wstring& value) {
	if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
	std::wstring out = L"\"";
	for (const wchar_t c : value) {
		if (c == L'"') out += L'\\';
		out += c;
	}
	out += L'"';
	return out;
}

bool file_exists(const std::wstring& path) {
	const DWORD attrs = GetFileAttributesW(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring directory_of(const std::wstring& path) {
	const size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos) return std::wstring();
	return path.substr(0, pos);
}

} // namespace

std::string utf8_from_wide(const std::wstring& text) {
	if (text.empty()) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		nullptr, 0, nullptr, nullptr);
	if (n <= 0) return std::string();
	std::string out(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n,
		nullptr, nullptr);
	return out;
}

std::wstring wide_from_utf8(const std::string& text) {
	if (text.empty()) return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		nullptr, 0);
	if (n <= 0) return std::wstring();
	std::wstring out(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
	return out;
}

bridge_process::bridge_process(std::wstring pipe_name) : m_pipe_name(std::move(pipe_name)) {}

bridge_process::~bridge_process() { release(); }

bridge_process& bridge_process::instance() {
	static bridge_process object;
	return object;
}

void bridge_process::set_cache_mb(int cache_mb) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_cache_mb = cache_mb;
}

void bridge_process::set_debug(bool debug) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_debug = debug;
}

void bridge_process::set_expected_version(std::string version) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_expected_version = std::move(version);
}

void bridge_process::set_warning_sink(std::function<void(const std::string&)> sink) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_warn = std::move(sink);
}

void bridge_process::warn_locked(const std::string& text) {
	if (m_warn) m_warn(text);
}

bool process_running(unsigned long pid) {
	if (pid == 0) return false;
	HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	if (h == nullptr) {
		// No such process; access denied and the like mean it exists.
		return GetLastError() != ERROR_INVALID_PARAMETER;
	}
	const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
	CloseHandle(h);
	return running;
}

bool wait_process_exit(std::intptr_t process, const cancel_token& ct, unsigned timeout_ms) {
	if (process == 0) return true;
	HANDLE h = reinterpret_cast<HANDLE>(process);
	const ULONGLONG start = GetTickCount64();
	for (;;) {
		if (WaitForSingleObject(h, 200) == WAIT_OBJECT_0) return true;
		if (ct.aborted()) return false;
		if (timeout_ms != 0 && GetTickCount64() - start >= timeout_ms) return false;
	}
}

void close_process_handle(std::intptr_t process) {
	if (process != 0) CloseHandle(reinterpret_cast<HANDLE>(process));
}

void bridge_process::set_exe_path(std::wstring exe_path) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_exe = std::move(exe_path);
}

std::wstring bridge_process::exe_path() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_exe;
}

bool bridge_process::process_alive_locked() {
	if (m_process == 0) return false;
	HANDLE h = reinterpret_cast<HANDLE>(m_process);
	if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) return true;

	DWORD code = 0;
	if (!GetExitCodeProcess(h, &code)) code = 0;
	m_exit_code = static_cast<unsigned long>(code);
	CloseHandle(h);
	m_process = 0;
	return false;
}

bool bridge_process::pipe_reachable_locked(unsigned timeout_ms) {
	const std::wstring path = L"\\\\.\\pipe\\" + m_pipe_name;
	const ULONGLONG start = GetTickCount64();

	for (;;) {
		DWORD slice = 0;
		if (timeout_ms != 0) {
			const ULONGLONG elapsed = GetTickCount64() - start;
			if (elapsed >= timeout_ms) return false;
			// WaitNamedPipe returns at once when no instance exists at all, so
			// this stays responsive while a bridge is still starting.
			slice = static_cast<DWORD>(std::min<ULONGLONG>(timeout_ms - elapsed, 50));
		}
		if (WaitNamedPipeW(path.c_str(), slice)) return true;
		if (timeout_ms == 0) return false;

		// WaitNamedPipe returns immediately when the pipe does not exist at
		// all, so sleep a little instead of spinning while it starts.
		const DWORD err = GetLastError();
		if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PIPE_NOT_CONNECTED) {
			Sleep(20);
		}
	}
}

bool bridge_process::pipe_reachable(unsigned timeout_ms) {
	std::lock_guard<std::mutex> lock(m_mutex);
	return pipe_reachable_locked(timeout_ms);
}

bool bridge_process::spawn_locked(std::string& error, bool new_console,
	std::intptr_t* login_process) {
	if (m_exe.empty()) {
		error = "librespot-bridge.exe path is not configured";
		return false;
	}
	if (!file_exists(m_exe)) {
		error = "librespot-bridge.exe not found: " + utf8_from_wide(m_exe);
		return false;
	}

	std::wstring cmd = quote_arg(m_exe);
	if (new_console) {
		cmd += L" login";
	} else {
		cmd += L" --parent-pid " + std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId()));
		cmd += L" --pipe " + m_pipe_name;
		if (m_cache_mb >= 0) cmd += L" --cache-mb " + std::to_wstring(m_cache_mb);
		if (m_debug) cmd += L" --debug";
	}

	std::vector<wchar_t> cmdBuffer(cmd.begin(), cmd.end());
	cmdBuffer.push_back(0);

	const std::wstring workDir = directory_of(m_exe);
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = new_console ? SW_SHOWNORMAL : SW_HIDE;
	PROCESS_INFORMATION pi = {};

	// No inherited handles: the bridge only needs its pipe name and the pid,
	// and it finds the pipe itself.
	const DWORD flags = new_console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
	if (!CreateProcessW(m_exe.c_str(), cmdBuffer.data(), nullptr, nullptr, FALSE, flags, nullptr,
			workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
		error = "CreateProcess failed: " + win32_message(GetLastError());
		return false;
	}

	if (new_console) {
		CloseHandle(pi.hThread);
		if (login_process != nullptr) {
			*login_process = reinterpret_cast<std::intptr_t>(pi.hProcess);
		} else {
			CloseHandle(pi.hProcess);
		}
	} else {
		if (m_process != 0) {
			CloseHandle(reinterpret_cast<HANDLE>(m_process));
		}
		CloseHandle(pi.hThread);
		m_process = reinterpret_cast<std::intptr_t>(pi.hProcess);
	}
	return true;
}

bool bridge_process::try_status(bridge_status& out, std::string& error, unsigned timeout_ms) {
	bridge_client client(m_pipe_name);
	cancel_token never;
	try {
		out = client.status(never, timeout_ms, timeout_ms + 1000);
		error.clear();
		return true;
	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}
}

bool bridge_process::probe(bridge_status& out, std::string& error, unsigned timeout_ms) {
	bridge_status status;
	std::string err;
	if (!try_status(status, err, timeout_ms)) {
		error = err;
		return false;
	}
	out = status;
	error.clear();
	return true;
}

void bridge_process::ensure_running(const cancel_token& ct, unsigned start_timeout_ms) {
	std::lock_guard<std::mutex> lock(m_mutex);
	ct.check();
	if (m_final_stop) throw cancelled(); // quitting: the bridge stays stopped

	// Pipe connectability is the whole readiness test. A status request used to
	// be the test, but status queues behind session creation while the network
	// is slow, which made a perfectly healthy bridge look dead.
	if (pipe_reachable_locked(0)) {
		if (process_alive_locked() || m_foreign_checked) return;
		// A bridge we did not start: check it once (it may be a leftover that is
		// about to exit, or another instance's bridge of another version).
		check_foreign_bridge_locked(ct);
		if (pipe_reachable_locked(0)) return;
	}
	m_foreign_checked = false;

	std::string error;
	if (!process_alive_locked()) {
		ct.check();
		if (!spawn_locked(error, false, nullptr)) {
			throw pipe_error("could not start librespot-bridge.exe: " + error);
		}
	}
	// If a bridge of ours is still alive, never start another one: wait for it
	// to open the pipe instead.

	const unsigned timeout = start_timeout_ms == 0 ? 15000 : start_timeout_ms;
	const ULONGLONG deadline = GetTickCount64() + timeout;
	for (;;) {
		ct.check();
		if (pipe_reachable_locked(100)) return;

		if (!process_alive_locked()) {
			throw pipe_error("librespot-bridge.exe exited during startup (exit code " +
				std::to_string(m_exit_code) +
				"); see %APPDATA%\\foo_librespot\\bridge.log");
		}
		if (GetTickCount64() >= deadline) {
			throw pipe_error("timed out waiting for librespot-bridge.exe to open its pipe");
		}
	}
}

void bridge_process::check_foreign_bridge_locked(const cancel_token& ct) {
	m_foreign_checked = true;
	bridge_status status;
	std::string error;
	if (!try_status(status, error, 1000)) return; // busy or old: just use it

	const unsigned long self = static_cast<unsigned long>(GetCurrentProcessId());
	if (status.parent_pid == self && status.pid != 0) {
		// Ours after all (the handle was released): adopt it, so quitting stops it.
		HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
			FALSE, static_cast<DWORD>(status.pid));
		if (h != nullptr) {
			if (m_process != 0) CloseHandle(reinterpret_cast<HANDLE>(m_process));
			m_process = reinterpret_cast<std::intptr_t>(h);
		}
		return;
	}

	if (status.parent_pid != 0 && !process_running(status.parent_pid)) {
		// Left over from a foobar2000 that has exited. It watches that process
		// and exits by itself; wait for its pipe to go away (up to 5 s) and start
		// our own, so a component update never runs against the old exe.
		const ULONGLONG deadline = GetTickCount64() + 5000;
		while (GetTickCount64() < deadline) {
			if (ct.aborted()) return;
			if (!pipe_reachable_locked(0)) return;
			Sleep(100);
		}
		warn_locked("a librespot-bridge.exe left over from an exited foobar2000 (process " +
			std::to_string(status.pid) + ") is still running; using it");
		return;
	}

	if (!m_expected_version.empty() && !status.version.empty() &&
		status.version != m_expected_version && !m_version_warned) {
		m_version_warned = true;
		const std::string owner = status.parent_pid != 0
			? "another foobar2000, process " + std::to_string(status.parent_pid)
			: "another program";
		warn_locked("the running librespot-bridge.exe is version " + status.version +
			" (started by " + owner + "), this component is " + m_expected_version +
			"; close the other foobar2000 to use the matching bridge");
	}
}

bool bridge_process::launch_login(std::string& error, std::intptr_t* process) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_exe.empty()) {
		error = "librespot-bridge.exe path is not configured";
		return false;
	}
	return spawn_locked(error, true, process);
}

bool bridge_process::owns_running_bridge() {
	std::lock_guard<std::mutex> lock(m_mutex);
	return process_alive_locked();
}

bridge_process::stop_result bridge_process::stop_owned(unsigned wait_ms, bool final_stop) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (final_stop) m_final_stop = true;
	if (!process_alive_locked()) return stop_result::not_ours;

	HANDLE h = reinterpret_cast<HANDLE>(m_process);
	bool asked = false;
	try {
		// The bridge checks that the caller is its --parent-pid, i.e. us.
		bridge_client(m_pipe_name).shutdown(cancel_token(), 500, 1500);
		asked = true;
	} catch (const std::exception&) {
		// Not answering (hung, or still starting): fall through to the wait.
	}

	stop_result result = stop_result::stopped;
	const DWORD wait = asked ? wait_ms : std::min(wait_ms, 1000u);
	if (WaitForSingleObject(h, wait) != WAIT_OBJECT_0) {
		// Last resort, and only for the process we created.
		result = TerminateProcess(h, 1) ? stop_result::terminated : stop_result::failed;
		WaitForSingleObject(h, 1000);
	}
	DWORD code = 0;
	if (GetExitCodeProcess(h, &code)) m_exit_code = code;
	CloseHandle(h);
	m_process = 0;
	return result;
}

void bridge_process::release() {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_process != 0) {
		CloseHandle(reinterpret_cast<HANDLE>(m_process));
		m_process = 0;
	}
}

} // namespace flp
