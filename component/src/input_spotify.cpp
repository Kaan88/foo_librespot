#include "stdafx.h"

#include "component_helpers.h"
#include "config.h"

#include "bridge/process.h"

#include <SDK/input_impl.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// 4096 frames per decode_run() call, as suggested by the protocol notes.
constexpr size_t kFramesPerChunk = 4096;

class input_spotify : public input_stubs {
public:
	void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason reason,
		abort_callback& aborter) {
		(void)p_filehint; // no filesystem object is involved for spotify: paths

		if (reason == input_open_info_write) throw exception_tagging_unsupported();
		if (!flp::is_spotify_track_path(p_path)) {
			throw exception_io_data("Not a Spotify track path (expected spotify:track:<id>)");
		}

		m_path = p_path;
		flp::ensure_bridge_started(aborter);

		try {
			flp::bridge_client client;
			const std::vector<flp::track_info> tracks =
				client.info({m_path}, flp::make_cancel(aborter));
			if (tracks.empty()) throw exception_io_data("Spotify: track not found");
			m_track = tracks.front();
			if (m_track.uri.empty()) m_track.uri = m_path;
			m_have_track = true;
		} catch (const flp::cancelled&) {
			throw exception_aborted();
		} catch (const std::exception& error) {
			flp::throw_io_error(error);
		}
	}

	void get_info(file_info& p_info, abort_callback& aborter) {
		aborter.check();
		if (!m_have_track) throw exception_io_data("Spotify: no track info available");
		flp::fill_file_info(m_track, p_info);
	}

	t_filestats2 get_stats2(uint32_t, abort_callback& aborter) {
		aborter.check();
		return t_filestats2(); // streams have no meaningful file stats
	}

	void decode_initialize(unsigned p_flags, abort_callback& aborter) {
		(void)p_flags;
		open_stream(0, aborter);
	}

	bool decode_run(audio_chunk& p_chunk, abort_callback& aborter) {
		if (m_eof || !m_stream) return false;

		const size_t frameBytes = static_cast<size_t>(m_channels) * sizeof(float);
		const size_t targetBytes = kFramesPerChunk * frameBytes;
		m_pcm.resize(targetBytes);

		const flp::cancel_token cancel = flp::make_cancel(aborter);
		while (m_pcmBytes.size() < targetBytes) {
			size_t got = 0;
			try {
				const size_t want = targetBytes - m_pcmBytes.size();
				if (!m_stream->read_some(m_pcm.data(), want, got, cancel)) {
					m_eof = true;
					break;
				}
			} catch (const flp::cancelled&) {
				throw exception_aborted();
			} catch (const std::exception& error) {
				flp::throw_io_error(error);
			}
			if (got == 0) {
				m_eof = true;
				break;
			}
			m_pcmBytes.insert(m_pcmBytes.end(), m_pcm.begin(), m_pcm.begin() + got);
		}

		const size_t frames = m_pcmBytes.size() / frameBytes;
		if (frames == 0) {
			m_stream.reset();
			return false;
		}

		p_chunk.set_data_32(reinterpret_cast<const float*>(m_pcmBytes.data()),
			static_cast<t_size>(frames), static_cast<unsigned>(m_channels),
			static_cast<unsigned>(m_sample_rate));

		const size_t consumed = frames * frameBytes;
		m_pcmBytes.erase(m_pcmBytes.begin(), m_pcmBytes.begin() + consumed);
		return true;
	}

	void decode_seek(double p_seconds, abort_callback& aborter) {
		if (p_seconds < 0) p_seconds = 0;
		long long startMs = static_cast<long long>(std::llround(p_seconds * 1000.0));
		if (m_track.duration_ms > 0 && startMs > m_track.duration_ms) startMs = m_track.duration_ms;

		if (m_stream) m_stream->close();
		m_stream.reset();
		open_stream(startMs, aborter);
	}

	bool decode_can_seek() { return true; }

	bool decode_get_dynamic_info(file_info& p_out, double& p_timestamp_delta) {
		(void)p_out;
		(void)p_timestamp_delta;
		return false;
	}

	bool decode_get_dynamic_info_track(file_info& p_out, double& p_timestamp_delta) {
		(void)p_out;
		(void)p_timestamp_delta;
		return false;
	}

	void decode_on_idle(abort_callback&) {}

	void retag(const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
	void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }

	static bool g_is_our_content_type(const char*) { return false; }

	static bool g_is_our_path(const char* p_path, const char* p_extension) {
		(void)p_extension;
		return flp::is_spotify_track_path(p_path);
	}

	static const char* g_get_name() { return "Spotify (foo_librespot)"; }

	static GUID g_get_guid() {
		static const GUID guid = { 0x2e6f7f19, 0x3f0b, 0x4d3a, { 0x9e, 0x62, 0x2a, 0x8c, 0x51, 0x7d, 0x1c, 0xb4 } };
		return guid;
	}

private:
	void open_stream(long long startMs, abort_callback& aborter) {
		flp::ensure_bridge_started(aborter);
		try {
			flp::bridge_client client;
			flp::open_result opened = client.open(m_path, startMs, flp::effective_bitrate_kbps(),
				flp::make_cancel(aborter));

			m_stream = std::move(opened.stream);
			m_sample_rate = opened.sample_rate > 0 ? opened.sample_rate : 44100;
			m_channels = opened.channels > 0 ? opened.channels : 2;
			if (!opened.track.uri.empty()) {
				m_track = opened.track;
				m_have_track = true;
			}
			m_eof = false;
			m_pcmBytes.clear();
		} catch (const flp::cancelled&) {
			throw exception_aborted();
		} catch (const std::exception& error) {
			flp::throw_io_error(error);
		}
	}

	std::string m_path;
	flp::track_info m_track;
	bool m_have_track = false;

	std::unique_ptr<flp::pipe_stream> m_stream;
	unsigned m_sample_rate = 44100;
	unsigned m_channels = 2;
	bool m_eof = false;
	std::vector<unsigned char> m_pcm;
	std::vector<unsigned char> m_pcmBytes;
};

static input_singletrack_factory_t<input_spotify> g_input_spotify_factory;

} // namespace
