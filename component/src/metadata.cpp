// SDK-only metadata mapping (no component services); compiled without the PCH
// so tools/metadata_probe can share it.

#include "metadata.h"

namespace flp {

void fill_file_info(const track_info& track, file_info& info) {
	if (track.duration_ms > 0) info.set_length(static_cast<double>(track.duration_ms) / 1000.0);

	info.info_set("codec", track.codec.empty() ? "Vorbis" : track.codec.c_str());
	info.info_set("encoding", "lossy");
	info.info_set_int("samplerate", 44100);
	info.info_set_int("channels", 2);
	if (track.bitrate > 0) info.info_set_bitrate(track.bitrate);

	if (!track.title.empty()) info.meta_add("TITLE", track.title.c_str());
	for (const std::string& artist : track.artists) {
		if (!artist.empty()) info.meta_add("ARTIST", artist.c_str());
	}
	if (!track.album.empty()) info.meta_add("ALBUM", track.album.c_str());
	if (!track.album_artist.empty()) info.meta_add("ALBUM ARTIST", track.album_artist.c_str());
	if (track.track_number > 0) {
		const pfc::format_int_t number = pfc::format_int(track.track_number);
		info.meta_add("TRACKNUMBER", number.c_str());
	}
	if (track.disc_number > 0) {
		const pfc::format_int_t number = pfc::format_int(track.disc_number);
		info.meta_add("DISCNUMBER", number.c_str());
	}
	if (!track.date.empty()) info.meta_add("DATE", track.date.c_str());

	replaygain_info rg;
	if (track.has_track_gain) rg.m_track_gain = static_cast<float>(track.track_gain_db);
	if (track.has_track_peak) rg.m_track_peak = static_cast<float>(track.track_peak);
	if (track.has_album_gain) rg.m_album_gain = static_cast<float>(track.album_gain_db);
	if (track.has_album_peak) rg.m_album_peak = static_cast<float>(track.album_peak);
	if (!rg.empty()) info.set_replaygain(rg);
}

} // namespace flp
