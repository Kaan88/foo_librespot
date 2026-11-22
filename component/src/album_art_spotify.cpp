#include "stdafx.h"

#include "component_helpers.h"

#include "bridge/process.h"

#include <SDK/album_art.h>
#include <SDK/album_art_helpers.h>
#include <SDK/console.h>
#include <SDK/http_client.h>

#include <memory>
#include <vector>

namespace {

class album_art_instance_spotify : public album_art_extractor_instance {
public:
	explicit album_art_instance_spotify(std::string coverUrl) : m_coverUrl(std::move(coverUrl)) {}

	album_art_data_ptr query(const GUID& what, abort_callback& aborter) override {
		if (what != album_art_ids::cover_front) throw exception_album_art_not_found();
		if (m_cached.is_valid()) return m_cached;
		if (m_coverUrl.empty()) throw exception_album_art_not_found();

		try {
			http_request::ptr request = http_client::get()->create_request("GET");
			file::ptr reply = request->run(m_coverUrl.c_str(), aborter);

			std::vector<unsigned char> bytes;
			unsigned char buffer[64 * 1024];
			for (;;) {
				aborter.check();
				const size_t got = reply->read(buffer, sizeof(buffer), aborter);
				if (got == 0) break;
				bytes.insert(bytes.end(), buffer, buffer + got);
				if (bytes.size() > 32u * 1024u * 1024u) break; // sanity limit
			}
			if (bytes.empty()) throw exception_album_art_not_found();

			m_cached = album_art_data_impl::g_create(bytes.data(), bytes.size());
			return m_cached;
		} catch (const exception_aborted&) {
			throw;
		} catch (const std::exception& error) {
			console::print("foo_librespot: album art download failed: ", error.what());
			throw exception_album_art_not_found();
		}
	}

private:
	std::string m_coverUrl;
	album_art_data_ptr m_cached;
};

class album_art_extractor_spotify : public album_art_extractor {
public:
	bool is_our_path(const char* p_path, const char* p_extension) override {
		(void)p_extension;
		return flp::is_spotify_track_path(p_path);
	}

	album_art_extractor_instance_ptr open(file_ptr p_filehint, const char* p_path,
		abort_callback& aborter) override {
		(void)p_filehint;

		try {
			flp::ensure_bridge_started(aborter);
			flp::bridge_client client;
			const std::vector<flp::track_info> tracks =
				client.info({p_path}, flp::make_cancel(aborter));
			if (tracks.empty() || tracks.front().cover_url.empty()) {
				throw exception_album_art_not_found();
			}
			return fb2k::service_new<album_art_instance_spotify>(tracks.front().cover_url);
		} catch (const flp::cancelled&) {
			throw exception_aborted();
		} catch (const exception_album_art_not_found&) {
			throw;
		} catch (const exception_aborted&) {
			throw;
		} catch (const std::exception& error) {
			console::print("foo_librespot: album art lookup failed: ", error.what());
			throw exception_album_art_not_found();
		}
	}
};

static service_factory_single_t<album_art_extractor_spotify> g_album_art_spotify_factory;

} // namespace
