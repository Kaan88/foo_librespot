// Reports Spotify tracks played in foobar2000 back to Spotify (recently played,
// play counts, recommendations) through the bridge's v6 report_play op.
//
// play_callback_static runs on the main thread: it only tracks what is
// playing and for how long. A finished play that counts (playback_rules.h) is
// sent once from a worker thread and never retried: a failure is logged and
// the play is dropped (see "Bridge writes must never be resent blindly").
// The preference "Report plays to Spotify" (cfg_report_plays) turns it off.

#include "stdafx.h"

#include "component_helpers.h"
#include "config.h"
#include "playback_rules.h"
#include "spotify_playlists.h"

#include "bridge/process.h"

#include <SDK/play_callback.h>
#include <SDK/playlist.h>

#include <chrono>
#include <cmath>
#include <optional>

namespace {

long long unix_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

flp::play_start start_from_command(play_control::t_track_command command) {
	switch (command) {
	case play_control::track_command_next:
	case play_control::track_command_rand: return flp::play_start::next;
	case play_control::track_command_prev: return flp::play_start::previous;
	case play_control::track_command_resume: return flp::play_start::resumed;
	default: return flp::play_start::clicked;
	}
}

flp::play_end end_from_command(play_control::t_track_command command) {
	switch (command) {
	case play_control::track_command_next:
	case play_control::track_command_rand: return flp::play_end::next;
	case play_control::track_command_prev: return flp::play_end::previous;
	default: return flp::play_end::other_track;
	}
}

// Main thread: the Spotify context of the playlist the track plays from.
std::string playing_context() {
	auto playlists = playlist_manager::get();
	t_size playlist = pfc_infinite, index = pfc_infinite;
	if (!playlists->get_playing_item_location(&playlist, &index)) return std::string();
	std::string context;
	if (!flp::playlist_context(playlist, context)) return std::string();
	// Radio and top-track snapshots carry a key, not a context Spotify knows.
	if (flp::is_static_playlist(playlist)) return std::string();
	return context;
}

bool shuffle_active() {
	auto playlists = playlist_manager::get();
	const char* name = playlists->playback_order_get_name(playlists->playback_order_get_active());
	if (name == nullptr) return false;
	const pfc::string8 order(name);
	return order.find_first("Shuffle") != pfc_infinite || order.find_first("Random") != pfc_infinite;
}

void send(flp::play_report report) {
	fb2k::splitTask([report = std::move(report)] {
		const flp::cancel_token cancel = flp::app_cancel_token();
		try {
			flp::init_bridge_paths();
			flp::bridge_process::instance().ensure_running(cancel);
			// Sent once. A timeout after the request left is not retried either:
			// Spotify may already have counted it.
			flp::bridge_client().report_play(report, cancel);
			FB2K_console_formatter() << "foo_librespot: reported play of " << report.uri.c_str()
				<< " (" << (report.ms_played / 1000) << " s, " << report.reason_end.c_str() << ")";
		} catch (const flp::cancelled&) {
		} catch (const std::exception& error) {
			FB2K_console_formatter() << "foo_librespot: could not report play of "
				<< report.uri.c_str() << " (not retried): " << error.what();
		}
	});
}

class play_reporter : public play_callback_static {
public:
	unsigned get_flags() override {
		return flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop |
			flag_on_playback_seek | flag_on_playback_time;
	}

	void on_playback_starting(play_control::t_track_command command, bool paused) override {
		(void)paused;
		m_command = command;
	}

	void on_playback_new_track(metadb_handle_ptr track) override {
		// Gapless advance has no stop: the previous track ran to its end.
		if (m_current) finish(m_command ? end_from_command(*m_command) : flp::play_end::track_done);

		const flp::play_start start = m_command ? start_from_command(*m_command)
			: (m_after_eof ? flp::play_start::track_done : flp::play_start::clicked);
		m_command.reset();
		m_after_eof = false;

		if (track.is_empty() || !flp::is_spotify_track_path(track->get_path())) return;
		current item;
		item.report.uri = track->get_path();
		item.report.started_at = unix_ms();
		item.report.reason_start = flp::reason_start_word(start);
		item.report.context_uri = playing_context();
		item.report.shuffle = shuffle_active();
		if (const auto info = track->get_info_ref(); info.is_valid()) {
			const double length = info->info().get_length();
			if (length > 0) item.report.duration_ms = static_cast<long long>(std::llround(length * 1000.0));
			if (const char* codec = info->info().info_get("codec")) item.report.audio_format = codec;
		}
		m_current = std::move(item);
	}

	void on_playback_stop(play_control::t_stop_reason reason) override {
		m_after_eof = reason == play_control::stop_reason_eof;
		// Only a switch to another track carries the starting command on to
		// on_playback_new_track; a stop or a failed start must not leave it to
		// mislabel the next natural advance.
		std::optional<play_control::t_track_command> command;
		if (reason == play_control::stop_reason_starting_another) command = m_command;
		else m_command.reset();
		if (!m_current) return;
		switch (reason) {
		case play_control::stop_reason_eof: finish(flp::play_end::track_done); break;
		case play_control::stop_reason_starting_another:
			finish(command ? end_from_command(*command) : flp::play_end::other_track);
			break;
		case play_control::stop_reason_shutting_down: finish(flp::play_end::quit); break;
		default: finish(flp::play_end::stopped); break;
		}
	}

	void on_playback_seek(double time) override {
		if (m_current) m_current->last_position = time;
	}

	// About once a second while playing; pauses and seeks leave gaps that are
	// not counted.
	void on_playback_time(double time) override {
		if (!m_current) return;
		const double delta = time - m_current->last_position;
		if (delta > 0 && delta <= 3.0) m_current->played_seconds += delta;
		m_current->last_position = time;
	}

	void on_playback_pause(bool) override {}
	void on_playback_edited(metadb_handle_ptr) override {}
	void on_playback_dynamic_info(const file_info&) override {}
	void on_playback_dynamic_info_track(const file_info&) override {}
	void on_volume_change(float) override {}

private:
	struct current {
		flp::play_report report;
		double last_position = 0;
		double played_seconds = 0;
	};

	void finish(flp::play_end end) {
		current item = std::move(*m_current);
		m_current.reset();
		item.report.ms_played = static_cast<long long>(std::llround(item.played_seconds * 1000.0));
		item.report.reason_end = flp::reason_end_word(end);
		const bool reached_end = end == flp::play_end::track_done;
		if (!flp::counts_as_play(item.report.ms_played, item.report.duration_ms, reached_end)) return;
		if (!flp::cfg_report_plays.get()) return;
		send(std::move(item.report));
	}

	std::optional<current> m_current;
	std::optional<play_control::t_track_command> m_command;
	bool m_after_eof = false;
};

FB2K_SERVICE_FACTORY(play_reporter);

} // namespace
