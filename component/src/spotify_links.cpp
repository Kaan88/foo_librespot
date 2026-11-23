#include "spotify_links.h"

#include <cctype>
#include <vector>

namespace flp {
namespace {

link_kind kind_from_word(const std::string& word) {
	if (word == "track") return link_kind::track;
	if (word == "album") return link_kind::album;
	if (word == "playlist") return link_kind::playlist;
	if (word == "artist") return link_kind::artist;
	return link_kind::none;
}

const char* kind_word(link_kind kind) {
	switch (kind) {
	case link_kind::track: return "track";
	case link_kind::album: return "album";
	case link_kind::playlist: return "playlist";
	case link_kind::artist: return "artist";
	default: return "";
	}
}

bool valid_id(const std::string& id) {
	if (id.empty() || id.size() > 64) return false;
	for (const char c : id) {
		if (!std::isalnum(static_cast<unsigned char>(c))) return false;
	}
	return true;
}

std::vector<std::string> split(const std::string& text, char separator) {
	std::vector<std::string> parts;
	std::string current;
	for (const char c : text) {
		if (c == separator) {
			parts.push_back(current);
			current.clear();
		} else {
			current.push_back(c);
		}
	}
	parts.push_back(current);
	return parts;
}

std::string trim(const std::string& text) {
	size_t begin = 0;
	size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
	return text.substr(begin, end - begin);
}

std::string lower(std::string text) {
	for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

spotify_link make_link(link_kind kind, const std::string& id) {
	spotify_link link;
	if (kind == link_kind::none || !valid_id(id)) return link;
	link.kind = kind;
	link.id = id;
	link.uri = std::string("spotify:") + kind_word(kind) + ":" + id;
	return link;
}

} // namespace

spotify_link parse_spotify_link(const std::string& input) {
	const std::string text = trim(input);

	if (text.compare(0, 8, "spotify:") == 0) {
		const std::vector<std::string> parts = split(text.substr(8), ':');
		// spotify:<kind>:<id>
		if (parts.size() == 2) return make_link(kind_from_word(parts[0]), parts[1]);
		// spotify:user:<name>:playlist:<id>
		if (parts.size() == 4 && parts[0] == "user" && !parts[1].empty() && parts[2] == "playlist") {
			return make_link(link_kind::playlist, parts[3]);
		}
		return spotify_link();
	}

	// URL form. Strip the scheme, then require the open.spotify.com host.
	std::string rest;
	const std::string lowered = lower(text);
	if (lowered.compare(0, 8, "https://") == 0) rest = text.substr(8);
	else if (lowered.compare(0, 7, "http://") == 0) rest = text.substr(7);
	else return spotify_link();

	const size_t slash = rest.find('/');
	if (slash == std::string::npos) return spotify_link();
	if (lower(rest.substr(0, slash)) != "open.spotify.com") return spotify_link();
	rest = rest.substr(slash + 1);

	// Drop query and fragment.
	const size_t cut = rest.find_first_of("?#");
	if (cut != std::string::npos) rest.erase(cut);

	std::vector<std::string> parts = split(rest, '/');
	while (!parts.empty() && parts.back().empty()) parts.pop_back();
	// Optional locale prefix, e.g. "intl-de".
	if (!parts.empty() && parts[0].compare(0, 5, "intl-") == 0) parts.erase(parts.begin());
	// Optional user prefix: user/<name>/playlist/<id>
	if (parts.size() == 4 && parts[0] == "user" && parts[2] == "playlist") {
		return make_link(link_kind::playlist, parts[3]);
	}
	if (parts.size() == 2) return make_link(kind_from_word(parts[0]), parts[1]);
	return spotify_link();
}

std::string spotify_web_url(const std::string& uri) {
	const spotify_link link = parse_spotify_link(uri);
	if (!link) return std::string();
	return std::string("https://open.spotify.com/") + kind_word(link.kind) + "/" + link.id;
}

} // namespace flp
