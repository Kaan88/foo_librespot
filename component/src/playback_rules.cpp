#include "playback_rules.h"

#include <algorithm>

namespace flp {

long long truncation_tolerance_ms(long long duration_ms) {
	return std::max<long long>(2000, duration_ms / 100);
}

long long missing_tail_ms(long long duration_ms, long long start_frame, long long frames,
	int sample_rate) {
	if (duration_ms <= 0 || sample_rate <= 0 || start_frame < 0 || frames < 0) return 0;
	const long long reachedMs = (start_frame + frames) * 1000 / sample_rate;
	const long long missing = duration_ms - reachedMs;
	return missing > truncation_tolerance_ms(duration_ms) ? missing : 0;
}

const char* reason_end_word(play_end end) {
	switch (end) {
	case play_end::track_done: return "trackdone";
	case play_end::next: return "fwdbtn";
	case play_end::previous: return "backbtn";
	case play_end::other_track: return "clickrow";
	case play_end::stopped: return "endplay";
	case play_end::quit: return "endplay";
	case play_end::error: return "trackerror";
	}
	return "unknown";
}

const char* reason_start_word(play_start start) {
	switch (start) {
	case play_start::track_done: return "trackdone";
	case play_start::next: return "fwdbtn";
	case play_start::previous: return "backbtn";
	case play_start::clicked: return "clickrow";
	case play_start::resumed: return "playbtn";
	}
	return "unknown";
}

bool counts_as_play(long long ms_played, long long duration_ms, bool reached_end) {
	if (ms_played < 1000) return false;
	if (ms_played >= 30000) return true;
	return reached_end && duration_ms > 0 && ms_played * 2 >= duration_ms;
}

bool rate_limiter::allow(const std::string& key, long long now_ms, unsigned& suppressed) {
	std::lock_guard<std::mutex> lock(m_mutex);
	entry& e = m_entries[key];
	if (e.used && now_ms - e.last_ms < m_interval_ms) {
		++e.held;
		suppressed = 0;
		return false;
	}
	suppressed = e.held;
	e.held = 0;
	e.last_ms = now_ms;
	e.used = true;
	return true;
}

} // namespace flp
