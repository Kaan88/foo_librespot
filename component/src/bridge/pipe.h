// Interruptible Windows named-pipe stream.
//
// The bridge protocol is request/response on one connection, then raw PCM until
// the server closes it. All I/O is overlapped so that a foobar2000
// abort_callback (wrapped in cancel_token) can interrupt it: the wait is done
// in short slices and CancelIoEx() is issued as soon as a cancel is observed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace flp {

// Thrown when the caller's abort_callback fired.
struct cancelled {};

struct pipe_error : std::runtime_error {
	explicit pipe_error(const std::string& what) : std::runtime_error(what) {}
};

// Thin, dependency-free view of an abort_callback.
struct cancel_token {
	std::function<bool()> is_aborting;

	bool aborted() const { return is_aborting && is_aborting(); }
	void check() const {
		if (aborted()) throw cancelled();
	}
};

class pipe_stream {
public:
	pipe_stream() = default;
	~pipe_stream();
	pipe_stream(const pipe_stream&) = delete;
	pipe_stream& operator=(const pipe_stream&) = delete;

	// Connects to \\.\pipe\<pipe_name>; waits up to timeout_ms for an instance.
	void connect(const std::wstring& pipe_name, unsigned timeout_ms, const cancel_token& ct);

	// Writes all bytes or throws.
	void write_all(const void* data, size_t bytes, const cancel_token& ct);

	// Reads one '\n'-terminated line (terminator stripped, optional '\r'
	// stripped). Throws pipe_error if the connection ends first.
	std::string read_line(const cancel_token& ct, unsigned timeout_ms = 30000);

	// Reads up to `bytes`. Returns false on end of stream. `got` may be zero
	// only when returning false.
	bool read_some(void* data, size_t bytes, size_t& got, const cancel_token& ct,
		unsigned timeout_ms = 30000);

	void close();
	bool valid() const { return m_pipe != kInvalid; }

private:
	static constexpr std::intptr_t kInvalid = -1;
	std::intptr_t m_pipe = kInvalid;
	std::intptr_t m_event = 0;
	std::vector<unsigned char> m_pending;

	// Issues one overlapped read and appends the bytes to m_pending.
	// Returns false on clean end of stream.
	bool read_chunk(size_t max_bytes, const cancel_token& ct, unsigned timeout_ms);
	bool read_direct(void* data, size_t bytes, size_t& got, const cancel_token& ct,
		unsigned timeout_ms);
	void wait_completion(void* overlapped, unsigned timeout_ms, const cancel_token& ct);
};

} // namespace flp
