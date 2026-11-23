// Unit test for the bridge -> file_info mapping (src/metadata.cpp).
//
// Links against the foobar2000 SDK static libraries only; the foobar2000 host
// is not needed. At runtime it needs shared.dll next to the executable (copied
// from a foobar2000 installation by scripts/run_tests.ps1).

#include "../../src/metadata.h"

#include <SDK/foobar2000.h>

#include <cmath>
#include <cstdio>
#include <cstring>

// The SDK's console/file_info helpers reference these host entry points; the
// test never reaches them, so simple stubs are enough to satisfy the linker.
namespace core_api {
bool are_services_available() { return false; }
bool is_main_thread() { return true; }
} // namespace core_api

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition) ++g_failures;
}

bool close_enough(double a, double b) { return std::fabs(a - b) < 1e-4; }

bool meta_is(const file_info& info, const char* name, const char* expected) {
	const char* actual = info.meta_get(name, 0);
	return actual != nullptr && std::strcmp(actual, expected) == 0;
}

void test_track_metadata() {
	flp::track_info track;
	track.uri = "spotify:track:0123456789abcdefghijkl";
	track.title = "Song Title";
	track.artists = { "Artist One", "Artist Two" };
	track.album = "Album Name";
	track.album_artist = "Album Artist";
	track.track_number = 3;
	track.disc_number = 2;
	track.date = "2020";
	track.duration_ms = 200040;
	track.codec = "vorbis";
	track.bitrate = 320;
	track.has_track_gain = true;
	track.track_gain_db = -5.64;
	track.has_track_peak = true;
	track.track_peak = 1.104;

	file_info_impl info;
	flp::fill_file_info(track, info);

	check(close_enough(info.get_length(), 200.04), "length = duration_ms / 1000");
	check(meta_is(info, "TITLE", "Song Title"), "TITLE");
	check(info.meta_get_count_by_name("ARTIST") == 2, "two ARTIST values");
	check(info.meta_get("ARTIST", 0) != nullptr &&
			std::strcmp(info.meta_get("ARTIST", 0), "Artist One") == 0, "ARTIST[0]");
	check(info.meta_get("ARTIST", 1) != nullptr &&
			std::strcmp(info.meta_get("ARTIST", 1), "Artist Two") == 0, "ARTIST[1]");
	check(meta_is(info, "ALBUM", "Album Name"), "ALBUM");
	check(meta_is(info, "ALBUM ARTIST", "Album Artist"), "ALBUM ARTIST");
	check(meta_is(info, "TRACKNUMBER", "3"), "TRACKNUMBER");
	check(meta_is(info, "DISCNUMBER", "2"), "DISCNUMBER");
	check(meta_is(info, "DATE", "2020"), "DATE");
	check(meta_is(info, "SOURCE", "Spotify"), "SOURCE=Spotify for now-playing scripts");

	const char* spotifyUri = info.info_get("spotify_uri");
	check(spotifyUri != nullptr && std::strcmp(spotifyUri, track.uri.c_str()) == 0,
		"spotify_uri technical field");

	const char* codec = info.info_get("codec");
	check(codec != nullptr && std::strcmp(codec, "vorbis") == 0, "codec");
	check(info.info_get_int("samplerate") == 44100, "samplerate 44100");
	check(info.info_get_int("channels") == 2, "channels 2");
	check(info.info_get_int("bitrate") == 320, "bitrate 320 kbps");

	const replaygain_info gains = info.get_replaygain();
	check(close_enough(gains.m_track_gain, -5.64), "REPLAYGAIN_TRACK_GAIN");
	check(close_enough(gains.m_track_peak, 1.104), "REPLAYGAIN_TRACK_PEAK");
	check(!gains.is_album_gain_present(), "album gain absent stays absent");
}

void test_album_gain_only() {
	flp::track_info track;
	track.title = "T";
	track.duration_ms = 1000;
	track.has_album_gain = true;
	track.album_gain_db = -18.2;
	track.has_album_peak = true;
	track.album_peak = 1.5;

	file_info_impl info;
	flp::fill_file_info(track, info);

	const replaygain_info gains = info.get_replaygain();
	check(!gains.is_track_gain_present(), "track gain absent stays absent");
	check(close_enough(gains.m_album_gain, -18.2), "REPLAYGAIN_ALBUM_GAIN");
	check(close_enough(gains.m_album_peak, 1.5), "REPLAYGAIN_ALBUM_PEAK");
}

void test_empty_fields_are_skipped() {
	flp::track_info track;
	track.duration_ms = 0;
	track.bitrate = 0;

	file_info_impl info;
	flp::fill_file_info(track, info);

	check(info.meta_get_count_by_name("ARTIST") == 0, "no ARTIST for empty artists");
	check(info.meta_get_count_by_name("TRACKNUMBER") == 0, "no TRACKNUMBER for 0");
	check(info.get_replaygain().empty(), "no ReplayGain when all four values absent");
	check(info.get_length() == 0.0, "no length for 0 duration");
}

} // namespace

int main() {
	test_track_metadata();
	test_album_gain_only();
	test_empty_fields_are_skipped();

	if (g_failures != 0) {
		std::printf("%d metadata test(s) FAILED\n", g_failures);
		return 1;
	}
	std::printf("All metadata tests passed\n");
	return 0;
}
