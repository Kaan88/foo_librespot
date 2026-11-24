// Small playback decisions with no foobar2000 dependency, so the console test
// tools can check them: truncated streams, when a play counts as a play, the
// reason words Spotify uses, and a rate limiter for console messages.

#pragma once

#include <map>
#include <mutex>
#include <string>

namespace flp {

// The tolerance for a stream that ends before the open header's duration:
// 2 s, or 1 % of the track for long ones. Spotify's duration and the audio
// file differ by a few hundred ms at most; a lost connection loses far more.
long long truncation_tolerance_ms(long long duration_ms);

// How much of the track (in ms) was missing when a stream ended cleanly after
// delivering `frames` frames from `start_frame`, measured against the header's
// `duration_ms`. 0 when the stream reached the end within the tolerance, or
// when the duration is unknown.
long long missing_tail_ms(long long duration_ms, long long start_frame, long long frames,
	int sample_rate);

// How a track's playback ended, as foobar2000 tells it.
enum class play_end {
	track_done,  // played to the end (next track followed, or end of playlist)
	next,        // the user skipped forward
	previous,    // the user went back
	other_track, // the user picked another track
	stopped,     // the user stopped playback
	quit,        // foobar2000 shut down
	error,       // playback failed
};

// How the next track's playback started.
enum class play_start {
	track_done,  // the previous one ended
	next,
	previous,
	clicked,     // the user picked it (double click, Play)
	resumed,     // Play after stop/pause
};

const char* reason_end_word(play_end end);
const char* reason_start_word(play_start start);

// Spotify counts a play from 30 s on. Shorter tracks count when they were
// played to the end and at least half of them was heard. Under 1 s never.
bool counts_as_play(long long ms_played, long long duration_ms, bool reached_end);

// Allows a message per key at most once per `interval_ms`, and counts the
// ones it held back, so a flood (album art failing for every row of a big
// playlist) shows up as one line plus "N more".
class rate_limiter {
public:
	explicit rate_limiter(long long interval_ms) : m_interval_ms(interval_ms) {}

	// True when a message for `key` may be printed now; `suppressed` is then
	// the number held back since the last one.
	bool allow(const std::string& key, long long now_ms, unsigned& suppressed);

private:
	struct entry {
		long long last_ms = 0;
		unsigned held = 0;
		bool used = false;
	};
	long long m_interval_ms;
	std::mutex m_mutex;
	std::map<std::string, entry> m_entries;
};

} // namespace flp
