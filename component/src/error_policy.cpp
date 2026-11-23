// SDK-free; compiled without the PCH so tools/sync_plan_test can share it.

#include "error_policy.h"

#include <mutex>
#include <set>

namespace flp {
namespace {

const char kConnectPrefix[] = "connecting to Spotify failed: ";

std::mutex g_reported_mutex;
std::set<std::string> g_reported;

bool starts_with(const std::string& text, const char* prefix) {
	return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

} // namespace

error_info describe_error(const std::string& code, const std::string& detail, int retry_after) {
	error_info out;
	out.kind = "other";

	if (code == "premium_required") {
		out.kind = code;
		out.message = "Spotify: Premium is required \xE2\x80\x94 this account is not Premium, "
					  "or Premium has expired";
		out.popup = true;
	} else if (code == "rate_limited") {
		out.kind = code;
		out.message = retry_after > 0
			? "Spotify: rate limited \xE2\x80\x94 try again in " + std::to_string(retry_after) + " s"
			: std::string("Spotify: rate limited \xE2\x80\x94 try again in a minute");
		out.popup = true;
	} else if (code == "region_unavailable") {
		out.kind = code;
		out.message = "Spotify: this track isn't available in your country";
		out.skip_track = true;
		out.popup = true;
	} else if (code == "track_unavailable") {
		out.kind = code;
		out.message = "Spotify: this track is no longer available";
		out.skip_track = true;
		out.popup = true;
	} else if (code == "not_logged_in") {
		out.kind = code;
		out.message = "Spotify: not logged in \xE2\x80\x94 Preferences > Tools > foo_librespot";
		out.popup = true;
	} else if (starts_with(detail, kConnectPrefix)) {
		// The bridge reports a failed session as "connecting to Spotify failed:
		// <reason>" with the generic "unavailable" code.
		out.kind = "connecting";
		out.message = "Spotify: connecting failed \xE2\x80\x94 " + detail.substr(sizeof(kConnectPrefix) - 1);
	} else {
		out.message = "Spotify: " + detail;
	}
	return out;
}

error_info describe_error(const std::exception& error) {
	if (const auto* failure = dynamic_cast<const bridge_error*>(&error)) {
		return describe_error(failure->code, failure->what(), failure->retry_after);
	}
	return describe_error(std::string(), error.what(), 0);
}

bool first_report_of_kind(const std::string& kind) {
	std::lock_guard<std::mutex> lock(g_reported_mutex);
	return g_reported.insert(kind).second;
}

void reset_reported_kinds() {
	std::lock_guard<std::mutex> lock(g_reported_mutex);
	g_reported.clear();
}

} // namespace flp
