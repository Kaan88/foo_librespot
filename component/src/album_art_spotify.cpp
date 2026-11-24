#include "stdafx.h"

#include "component_helpers.h"
#include "playback_rules.h"

#include "bridge/process.h"

#include <SDK/album_art.h>
#include <SDK/album_art_helpers.h>
#include <SDK/console.h>
#include <SDK/http_client.h>

#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

long long now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One console line per kind of failure and minute; the rest is counted. A big
// playlist without network used to print one line per row.
void print_failure(const char* kind, const char* what) {
	static flp::rate_limiter limiter(60 * 1000);
	unsigned suppressed = 0;
	if (!limiter.allow(kind, now_ms(), suppressed)) return;
	pfc::string_formatter text;
	text << "foo_librespot: " << kind << " failed: " << what;
	if (suppressed > 0) text << " (" << suppressed << " similar messages suppressed)";
	console::print(text);
}

// Starts the bridge without the console line ensure_bridge_started prints:
// failures here go through print_failure.
void start_bridge(abort_callback& aborter) {
	flp::init_bridge_paths();
	flp::bridge_process::instance().ensure_running(flp::make_cancel(aborter));
}

// Shared by all extractor instances (every playlist row opens its own): the
// downloaded images by URL, what each track's cover and artist are, and the
// artists' image URLs. Function-local statics, so nothing is built during
// DLL load; album_art_data_ptr is created on first use only.
class art_cache {
public:
	static art_cache& get() {
		static art_cache cache;
		return cache;
	}

	struct track_art {
		std::string cover_url;
		std::string artist_uri;
	};

	bool find_track(const std::string& path, track_art& out) {
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto it = m_tracks.find(path);
		if (it == m_tracks.end()) return false;
		out = it->second;
		return true;
	}

	void put_track(const std::string& path, const track_art& art) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_tracks.size() >= kMaxTracks) m_tracks.clear();
		m_tracks[path] = art;
	}

	// Empty string cached = the artist has no image.
	bool find_artist(const std::string& uri, std::string& url) {
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto it = m_artists.find(uri);
		if (it == m_artists.end()) return false;
		url = it->second;
		return true;
	}

	void put_artist(const std::string& uri, const std::string& url) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_artists.size() >= kMaxArtists) m_artists.clear();
		m_artists[uri] = url;
	}

	album_art_data_ptr find_image(const std::string& url) {
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto it = m_images.find(url);
		if (it == m_images.end()) return album_art_data_ptr();
		m_lru.splice(m_lru.begin(), m_lru, it->second.position);
		return it->second.data;
	}

	void put_image(const std::string& url, const album_art_data_ptr& data) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_images.count(url) != 0) return;
		m_lru.push_front(url);
		m_images[url] = image{data, m_lru.begin()};
		m_bytes += data->get_size();
		while (!m_lru.empty() && (m_images.size() > kMaxImages || m_bytes > kMaxBytes)) {
			const auto victim = m_images.find(m_lru.back());
			m_bytes -= victim->second.data->get_size();
			m_images.erase(victim);
			m_lru.pop_back();
		}
	}

	// A URL that failed is not retried for a while.
	bool recently_failed(const std::string& url) {
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto it = m_failed.find(url);
		return it != m_failed.end() && now_ms() - it->second < kRetryMs;
	}

	void put_failed(const std::string& url) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_failed.size() >= kMaxTracks) m_failed.clear();
		m_failed[url] = now_ms();
	}

private:
	static constexpr size_t kMaxImages = 96;
	static constexpr size_t kMaxBytes = 48u * 1024u * 1024u;
	static constexpr size_t kMaxTracks = 4096;
	static constexpr size_t kMaxArtists = 1024;
	static constexpr long long kRetryMs = 5 * 60 * 1000;

	struct image {
		album_art_data_ptr data;
		std::list<std::string>::iterator position;
	};

	std::mutex m_mutex;
	std::unordered_map<std::string, track_art> m_tracks;
	std::unordered_map<std::string, std::string> m_artists;
	std::unordered_map<std::string, image> m_images;
	std::unordered_map<std::string, long long> m_failed;
	std::list<std::string> m_lru;
	size_t m_bytes = 0;
};

album_art_data_ptr download(const std::string& url, abort_callback& aborter) {
	art_cache& cache = art_cache::get();
	if (album_art_data_ptr cached = cache.find_image(url); cached.is_valid()) return cached;
	if (cache.recently_failed(url)) throw exception_album_art_not_found();

	try {
		http_request::ptr request = http_client::get()->create_request("GET");
		file::ptr reply = request->run(url.c_str(), aborter);

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

		album_art_data_ptr data = album_art_data_impl::g_create(bytes.data(), bytes.size());
		cache.put_image(url, data);
		return data;
	} catch (const exception_aborted&) {
		throw;
	} catch (const exception_album_art_not_found&) {
		cache.put_failed(url);
		throw;
	} catch (const std::exception& error) {
		cache.put_failed(url);
		print_failure("album art download", error.what());
		throw exception_album_art_not_found();
	}
}

class album_art_instance_spotify : public album_art_extractor_instance {
public:
	explicit album_art_instance_spotify(art_cache::track_art art) : m_art(std::move(art)) {}

	album_art_data_ptr query(const GUID& what, abort_callback& aborter) override {
		if (what == album_art_ids::cover_front) {
			if (m_art.cover_url.empty()) throw exception_album_art_not_found();
			return download(m_art.cover_url, aborter);
		}
		if (what == album_art_ids::artist) {
			const std::string url = artist_image_url(aborter);
			if (url.empty()) throw exception_album_art_not_found();
			return download(url, aborter);
		}
		throw exception_album_art_not_found();
	}

private:
	// The first artist's picture, from the bridge's artist page (cached).
	std::string artist_image_url(abort_callback& aborter) {
		if (m_art.artist_uri.empty()) return std::string();
		art_cache& cache = art_cache::get();
		std::string url;
		if (cache.find_artist(m_art.artist_uri, url)) return url;
		try {
			start_bridge(aborter);
			const flp::artist_result artist =
				flp::bridge_client().artist(m_art.artist_uri, flp::make_cancel(aborter));
			url = artist.artist.image_url;
			cache.put_artist(m_art.artist_uri, url);
			return url;
		} catch (const flp::cancelled&) {
			throw exception_aborted();
		} catch (const exception_aborted&) {
			throw;
		} catch (const std::exception& error) {
			print_failure("artist picture lookup", error.what());
			return std::string();
		}
	}

	art_cache::track_art m_art;
};

class album_art_extractor_spotify : public album_art_extractor_v2 {
public:
	bool is_our_path(const char* p_path, const char* p_extension) override {
		(void)p_extension;
		return flp::is_spotify_track_path(p_path);
	}

	GUID get_guid() override { return flp::input_spotify_guid(); }

	album_art_extractor_instance_ptr open(file_ptr p_filehint, const char* p_path,
		abort_callback& aborter) override {
		(void)p_filehint;

		art_cache::track_art art;
		if (!art_cache::get().find_track(p_path, art)) {
			try {
				start_bridge(aborter);
				flp::bridge_client client;
				const std::vector<flp::track_info> tracks =
					client.info({p_path}, flp::make_cancel(aborter));
				if (!tracks.empty()) {
					art.cover_url = tracks.front().cover_url;
					if (!tracks.front().artist_uris.empty()) {
						art.artist_uri = tracks.front().artist_uris.front();
					}
				}
				art_cache::get().put_track(p_path, art);
			} catch (const flp::cancelled&) {
				throw exception_aborted();
			} catch (const exception_aborted&) {
				throw;
			} catch (const std::exception& error) {
				print_failure("album art lookup", error.what());
				throw exception_album_art_not_found();
			}
		}
		if (art.cover_url.empty() && art.artist_uri.empty()) throw exception_album_art_not_found();
		return fb2k::service_new<album_art_instance_spotify>(art);
	}
};

static service_factory_single_t<album_art_extractor_spotify> g_album_art_spotify_factory;

} // namespace
