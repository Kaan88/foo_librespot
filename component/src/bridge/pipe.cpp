#include "pipe.h"

#include <algorithm>
#include <memory>

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

bool is_eof_error(DWORD code) {
	return code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED ||
		code == ERROR_HANDLE_EOF || code == ERROR_NO_DATA;
}

// Poll interval for abort checks while waiting for overlapped I/O.
constexpr DWORD kWaitSliceMs = 40;

} // namespace

pipe_stream::~pipe_stream() { close(); }

void pipe_stream::close() {
	if (m_event != 0) {
		CloseHandle(reinterpret_cast<HANDLE>(m_event));
		m_event = 0;
	}
	if (m_pipe != kInvalid) {
		CloseHandle(reinterpret_cast<HANDLE>(m_pipe));
		m_pipe = kInvalid;
	}
	m_pending.clear();
}

void pipe_stream::connect(const std::wstring& pipe_name, unsigned timeout_ms,
	const cancel_token& ct) {
	close();

	const std::wstring path = L"\\\\.\\pipe\\" + pipe_name;
	const ULONGLONG start = GetTickCount64();

	for (;;) {
		ct.check();

		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
			OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			m_pipe = reinterpret_cast<std::intptr_t>(h);
			m_event = reinterpret_cast<std::intptr_t>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
			if (m_event == 0) {
				close();
				throw pipe_error("could not create an I/O event object");
			}
			return;
		}

		const DWORD err = GetLastError();
		const ULONGLONG elapsed = GetTickCount64() - start;
		if (timeout_ms != 0 && elapsed >= timeout_ms) {
			if (err == ERROR_FILE_NOT_FOUND) {
				throw pipe_error("librespot bridge is not running (pipe not found)");
			}
			if (err == ERROR_PIPE_BUSY) {
				throw pipe_error("timed out waiting for an instance of the librespot bridge pipe");
			}
			throw pipe_error("could not connect to the librespot bridge: " + win32_message(err));
		}

		if (err == ERROR_PIPE_BUSY) {
			DWORD left = 100;
			if (timeout_ms != 0) {
				const ULONGLONG remain = timeout_ms > elapsed ? timeout_ms - elapsed : 0;
				left = static_cast<DWORD>(std::min<ULONGLONG>(left, remain));
			}
			WaitNamedPipeW(path.c_str(), left);
		} else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PIPE_NOT_CONNECTED) {
			Sleep(30);
		} else {
			throw pipe_error("could not connect to the librespot bridge: " + win32_message(err));
		}
	}
}

void pipe_stream::wait_completion(void* overlapped, unsigned timeout_ms, const cancel_token& ct) {
	HANDLE event = reinterpret_cast<HANDLE>(m_event);
	const ULONGLONG start = GetTickCount64();

	for (;;) {
		if (ct.aborted()) {
			CancelIoEx(reinterpret_cast<HANDLE>(m_pipe), reinterpret_cast<LPOVERLAPPED>(overlapped));
			DWORD ignored = 0;
			GetOverlappedResult(reinterpret_cast<HANDLE>(m_pipe),
				reinterpret_cast<LPOVERLAPPED>(overlapped), &ignored, TRUE);
			ResetEvent(event);
			throw cancelled();
		}

		DWORD slice = kWaitSliceMs;
		if (timeout_ms != 0) {
			const ULONGLONG elapsed = GetTickCount64() - start;
			if (elapsed >= timeout_ms) {
				CancelIoEx(reinterpret_cast<HANDLE>(m_pipe),
					reinterpret_cast<LPOVERLAPPED>(overlapped));
				DWORD ignored = 0;
				GetOverlappedResult(reinterpret_cast<HANDLE>(m_pipe),
					reinterpret_cast<LPOVERLAPPED>(overlapped), &ignored, TRUE);
				ResetEvent(event);
				throw pipe_error("timed out waiting for the librespot bridge");
			}
			slice = static_cast<DWORD>(std::min<ULONGLONG>(slice, timeout_ms - elapsed));
		}

		const DWORD w = WaitForSingleObject(event, slice);
		if (w == WAIT_OBJECT_0) return;
		if (w == WAIT_FAILED) {
			throw pipe_error("WaitForSingleObject failed: " +
				win32_message(GetLastError()));
		}
	}
}

void pipe_stream::write_all(const void* data, size_t bytes, const cancel_token& ct) {
	if (!valid()) throw pipe_error("write on a closed bridge connection");
	HANDLE pipe = reinterpret_cast<HANDLE>(m_pipe);
	HANDLE event = reinterpret_cast<HANDLE>(m_event);

	const unsigned char* p = static_cast<const unsigned char*>(data);
	size_t left = bytes;
	while (left > 0) {
		ct.check();

		const DWORD want = static_cast<DWORD>(std::min<size_t>(left, 1u << 20));
		OVERLAPPED ov = {};
		ov.hEvent = event;
		ResetEvent(event);

		DWORD written = 0;
		if (!WriteFile(pipe, p, want, &written, &ov)) {
			const DWORD err = GetLastError();
			if (err != ERROR_IO_PENDING) {
				throw pipe_error("write to the librespot bridge failed: " + win32_message(err));
			}
			wait_completion(&ov, 30000, ct);
			if (!GetOverlappedResult(pipe, &ov, &written, FALSE)) {
				const DWORD e = GetLastError();
				if (e == ERROR_OPERATION_ABORTED) throw cancelled();
				throw pipe_error("write to the librespot bridge failed: " + win32_message(e));
			}
		}
		if (written == 0) throw pipe_error("write to the librespot bridge returned zero bytes");
		p += written;
		left -= written;
	}
}

bool pipe_stream::read_chunk(size_t max_bytes, const cancel_token& ct, unsigned timeout_ms) {
	if (!valid()) throw pipe_error("read on a closed bridge connection");
	HANDLE pipe = reinterpret_cast<HANDLE>(m_pipe);
	HANDLE event = reinterpret_cast<HANDLE>(m_event);

	const size_t oldSize = m_pending.size();
	m_pending.resize(oldSize + max_bytes);

	OVERLAPPED ov = {};
	ov.hEvent = event;
	ResetEvent(event);

	DWORD got = 0;
	if (!ReadFile(pipe, m_pending.data() + oldSize, static_cast<DWORD>(max_bytes), &got, &ov)) {
		const DWORD err = GetLastError();
		if (err != ERROR_IO_PENDING) {
			m_pending.resize(oldSize);
			if (is_eof_error(err)) return false;
			if (err == ERROR_OPERATION_ABORTED) throw cancelled();
			throw pipe_error("read from the librespot bridge failed: " + win32_message(err));
		}
		wait_completion(&ov, timeout_ms, ct);
		if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) {
			const DWORD e = GetLastError();
			m_pending.resize(oldSize);
			if (is_eof_error(e)) return false;
			if (e == ERROR_OPERATION_ABORTED) throw cancelled();
			throw pipe_error("read from the librespot bridge failed: " + win32_message(e));
		}
	}

	if (got == 0) {
		m_pending.resize(oldSize);
		return false;
	}
	m_pending.resize(oldSize + got);
	return true;
}

bool pipe_stream::read_direct(void* data, size_t bytes, size_t& got, const cancel_token& ct,
	unsigned timeout_ms) {
	if (!valid()) throw pipe_error("read on a closed bridge connection");
	got = 0;
	if (bytes == 0) return true;

	HANDLE pipe = reinterpret_cast<HANDLE>(m_pipe);
	HANDLE event = reinterpret_cast<HANDLE>(m_event);

	OVERLAPPED ov = {};
	ov.hEvent = event;
	ResetEvent(event);

	const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 20));
	DWORD n = 0;
	if (!ReadFile(pipe, data, want, &n, &ov)) {
		const DWORD err = GetLastError();
		if (err != ERROR_IO_PENDING) {
			if (is_eof_error(err)) return false;
			if (err == ERROR_OPERATION_ABORTED) throw cancelled();
			throw pipe_error("read from the librespot bridge failed: " + win32_message(err));
		}
		wait_completion(&ov, timeout_ms, ct);
		if (!GetOverlappedResult(pipe, &ov, &n, FALSE)) {
			const DWORD e = GetLastError();
			if (is_eof_error(e)) return false;
			if (e == ERROR_OPERATION_ABORTED) throw cancelled();
			throw pipe_error("read from the librespot bridge failed: " + win32_message(e));
		}
	}
	if (n == 0) return false;
	got = n;
	return true;
}

bool pipe_stream::read_some(void* data, size_t bytes, size_t& got, const cancel_token& ct,
	unsigned timeout_ms) {
	got = 0;
	if (bytes == 0) return true;

	if (!m_pending.empty()) {
		const size_t n = std::min(bytes, m_pending.size());
		memcpy(data, m_pending.data(), n);
		m_pending.erase(m_pending.begin(), m_pending.begin() + n);
		got = n;
		return true;
	}
	return read_direct(data, bytes, got, ct, timeout_ms);
}

std::string pipe_stream::read_line(const cancel_token& ct, unsigned timeout_ms) {
	const ULONGLONG start = GetTickCount64();
	for (;;) {
		const auto it = std::find(m_pending.begin(), m_pending.end(), static_cast<unsigned char>('\n'));
		if (it != m_pending.end()) {
			const size_t len = static_cast<size_t>(it - m_pending.begin());
			std::string line(reinterpret_cast<const char*>(m_pending.data()), len);
			m_pending.erase(m_pending.begin(), m_pending.begin() + len + 1);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			return line;
		}
		if (m_pending.size() > 8u * 1024u * 1024u) {
			throw pipe_error("librespot bridge response is unreasonably large");
		}

		unsigned budget = timeout_ms;
		if (timeout_ms != 0) {
			const ULONGLONG elapsed = GetTickCount64() - start;
			if (elapsed >= timeout_ms) throw pipe_error("timed out waiting for a bridge reply");
			budget = static_cast<unsigned>(timeout_ms - elapsed);
		}
		if (!read_chunk(4096, ct, budget)) {
			throw pipe_error("the librespot bridge closed the connection before replying");
		}
	}
}

} // namespace flp
