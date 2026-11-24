// SDK-only metadata mapping (no component services); compiled without the PCH
// so tools/metadata_probe can share it.

#include "metadata.h"

namespace flp {

std::string codec_display_name(const std::string& codec) {
	if (codec.empty()) return "Vorbis";
	std::string lower;
	for (const char c : codec) lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	// foobar2000's own decoders name their codecs like this.
	if (lower == "vorbis" || lower == "ogg vorbis") return "Vorbis";
	if (lower == "flac") return "FLAC";
	if (lower == "mp3") return "MP3";
	if (lower == "aac") return "AAC";
	if (lower == "opus") return "Opus";
	return codec;
}

void fill_file_info(const track_info& track, file_info& info) {
	if (track.duration_ms > 0) info.set_length(static_cast<double>(track.duration_ms) / 1000.0);

	info.info_set("codec", codec_display_name(track.codec).c_str());
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

	// Readable by now-playing scripts: %source% and $info(spotify_uri).
	info.meta_add("SOURCE", "Spotify");
	if (!track.uri.empty()) info.info_set("spotify_uri", track.uri.c_str());
	// $info(spotify_album_uri) / $info(spotify_artist_uri); several artists are
	// joined with ", " in ARTIST order.
	if (!track.album_uri.empty()) info.info_set("spotify_album_uri", track.album_uri.c_str());
	if (!track.artist_uris.empty()) {
		std::string artists;
		for (const std::string& uri : track.artist_uris) {
			if (uri.empty()) continue;
			if (!artists.empty()) artists += ", ";
			artists += uri;
		}
		if (!artists.empty()) info.info_set("spotify_artist_uri", artists.c_str());
	}

	replaygain_info rg;
	if (track.has_track_gain) rg.m_track_gain = static_cast<float>(track.track_gain_db);
	if (track.has_track_peak) rg.m_track_peak = static_cast<float>(track.track_peak);
	if (track.has_album_gain) rg.m_album_gain = static_cast<float>(track.album_gain_db);
	if (track.has_album_peak) rg.m_album_peak = static_cast<float>(track.album_peak);
	if (!rg.empty()) info.set_replaygain(rg);
}

} // namespace flp
