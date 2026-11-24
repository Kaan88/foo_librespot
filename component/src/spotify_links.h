// Parsing of Spotify links: spotify: URIs and open.spotify.com URLs.
// Pure string functions (no SDK), unit-tested by tools/sync_plan_test.

#pragma once

#include <string>

namespace flp {

enum class link_kind { none, track, album, playlist, artist };

struct spotify_link {
	link_kind kind = link_kind::none;
	std::string id;
	std::string uri; // canonical: spotify:<kind>:<id>

	explicit operator bool() const { return kind != link_kind::none; }
};

// Accepts spotify:track|album|playlist|artist:ID, spotify:user:NAME:playlist:ID
// and https://open.spotify.com/[intl-xx/][user/NAME/]track|album|playlist|artist/ID[?query].
// Surrounding whitespace is ignored. Returns kind none for anything else.
spotify_link parse_spotify_link(const std::string& text);

// "https://open.spotify.com/track/<id>" for a track/album/playlist/artist URI,
// ".../collection/tracks" for spotify:user:<name>:collection (Liked Songs), else empty.
std::string spotify_web_url(const std::string& uri);

} // namespace flp
