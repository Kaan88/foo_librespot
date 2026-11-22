// Console harness for the librespot-bridge client (no foobar2000 needed).
//
//   bridge_probe --pipe NAME status
//   bridge_probe --pipe NAME info spotify:track:ID
//   bridge_probe --pipe NAME resolve spotify:album:ID
//   bridge_probe --pipe NAME play spotify:track:ID [--start-ms N] [--frames N|all]
//   bridge_probe --pipe NAME play ... --abort-after-ms N
//   bridge_probe --spawn DIR [--pipe NAME]     (spawns DIR\librespot-bridge.exe)
//
// Exit codes: 0 ok, 1 error, 2 usage, 3 cancelled, 4 verification failed.

#include "../src/bridge/process.h"
#include "../src/bridge/protocol.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

flp::cancel_token never_cancel() { return flp::cancel_token(); }

flp::cancel_token deadline_cancel(unsigned milliseconds) {
	flp::cancel_token token;
	if (milliseconds == 0) return token;
	const auto start = std::chrono::steady_clock::now();
	token.is_aborting = [start, milliseconds] {
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		return elapsed >= static_cast<long long>(milliseconds);
	};
	return token;
}

void print_status(const flp::bridge_status& status) {
	std::printf("status ok=true logged_in=%s username=%s country=%s version=%s\n",
		status.logged_in ? "true" : "false", status.username.c_str(), status.country.c_str(),
		status.version.c_str());
}

void print_track(const flp::track_info& track) {
	std::printf("track uri=%s title=%s artists=%zu album=%s duration_ms=%lld bitrate=%d "
				"gain=%s peak=%s\n",
		track.uri.c_str(), track.title.c_str(), track.artists.size(), track.album.c_str(),
		track.duration_ms, track.bitrate, track.has_track_gain ? "yes" : "no",
		track.has_track_peak ? "yes" : "no");
}

int run_status(flp::bridge_client& client) {
	print_status(client.status(never_cancel()));
	return 0;
}

int run_info(flp::bridge_client& client, const std::vector<std::string>& uris) {
	const std::vector<flp::track_info> tracks = client.info(uris, never_cancel());
	std::printf("info count=%zu\n", tracks.size());
	for (const flp::track_info& track : tracks) print_track(track);
	return tracks.empty() ? 4 : 0;
}

int run_resolve(flp::bridge_client& client, const std::string& uri) {
	const std::vector<flp::track_info> tracks = client.resolve(uri, never_cancel());
	std::printf("resolve count=%zu\n", tracks.size());
	for (const flp::track_info& track : tracks) print_track(track);
	return tracks.empty() ? 4 : 0;
}

int run_play(flp::bridge_client& client, const std::string& uri, int startMs, long long frameLimit,
	long long expectStartFrame, long long expectFrames, unsigned abortAfterMs) {
	const auto wallStart = std::chrono::steady_clock::now();
	flp::open_result opened =
		client.open(uri, startMs, 320, deadline_cancel(abortAfterMs));

	std::printf("header rate=%d channels=%d start_frame=%lld title=%s\n", opened.sample_rate,
		opened.channels, opened.start_frame, opened.track.title.c_str());
	if (expectStartFrame >= 0 && opened.start_frame != expectStartFrame) {
		std::printf("FAIL expected start_frame %lld, got %lld\n", expectStartFrame,
			opened.start_frame);
		return 4;
	}

	const size_t frameBytes = static_cast<size_t>(opened.channels) * sizeof(float);
	const size_t framesPerChunk = 4096;
	std::vector<unsigned char> buffer(framesPerChunk * frameBytes);
	std::vector<unsigned char> carry;

	long long frames = 0;
	double peak = 0.0;
	double sumSquares = 0.0;
	long long sampleValues = 0;
	bool channelMismatch = false;

	const flp::cancel_token cancel = deadline_cancel(abortAfterMs);

	try {
		for (;;) {
			if (cancel.aborted()) throw flp::cancelled();
			if (frameLimit > 0 && frames >= frameLimit) break;

			size_t got = 0;
			if (!opened.stream->read_some(buffer.data(), buffer.size(), got, cancel)) break;
			if (got == 0) break;

			carry.insert(carry.end(), buffer.begin(), buffer.begin() + got);
			const size_t wholeBytes = (carry.size() / frameBytes) * frameBytes;
			const size_t wholeFrames = wholeBytes / frameBytes;
			if (wholeFrames == 0) continue;

			const float* samples = reinterpret_cast<const float*>(carry.data());
			for (size_t f = 0; f < wholeFrames; ++f) {
				const float* frame = samples + f * static_cast<size_t>(opened.channels);
				for (int ch = 0; ch < opened.channels; ++ch) {
					const double v = frame[ch];
					if (!std::isfinite(v)) {
						std::printf("FAIL non-finite sample\n");
						return 4;
					}
					const double a = std::fabs(v);
					if (a > peak) peak = a;
					sumSquares += v * v;
					++sampleValues;
				}
				for (int ch = 1; ch < opened.channels; ++ch) {
					if (frame[ch] != frame[0]) channelMismatch = true;
				}
			}
			frames += static_cast<long long>(wholeFrames);
			carry.erase(carry.begin(), carry.begin() + wholeBytes);
		}
	} catch (const flp::cancelled&) {
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - wallStart).count();
		std::printf("cancelled frames=%lld after_ms=%lld\n", frames, static_cast<long long>(elapsed));
		if (elapsed > 2000) {
			std::printf("FAIL cancellation took too long\n");
			return 4;
		}
		return 3;
	}

	const double rms = sampleValues > 0 ? std::sqrt(sumSquares / sampleValues) : 0.0;
	std::printf("pcm frames=%lld samples=%lld peak=%.6f rms=%.6f channel_mismatch=%s\n", frames,
		sampleValues, peak, rms, channelMismatch ? "yes" : "no");

	if (expectFrames >= 0 && frames != expectFrames) {
		std::printf("FAIL expected %lld frames, got %lld\n", expectFrames, frames);
		return 4;
	}
	if (sampleValues > 0 && (peak < 0.2 || peak > 0.3)) {
		std::printf("FAIL sine peak outside [0.2, 0.3]\n");
		return 4;
	}
	if (sampleValues > 0 && (rms < 0.15 || rms > 0.2)) {
		std::printf("FAIL sine RMS outside [0.15, 0.2]\n");
		return 4;
	}
	if (channelMismatch) {
		std::printf("FAIL channels differ\n");
		return 4;
	}
	if (frames == 0) {
		std::printf("FAIL no audio received\n");
		return 4;
	}
	return 0;
}

int run_spawn(const std::wstring& directory, const std::wstring& pipeName) {
	flp::bridge_process process(pipeName);
	process.set_exe_path(directory + L"\\librespot-bridge.exe");
	const auto start = std::chrono::steady_clock::now();
	process.ensure_running(never_cancel(), 12000);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
	std::printf("spawn ok started_in_ms=%lld\n", static_cast<long long>(elapsed));

	flp::bridge_client client(pipeName);
	print_status(client.status(never_cancel()));
	return 0;
}

void usage() {
	std::printf("usage: bridge_probe [--pipe NAME] <status|info|resolve|play|spawn> ...\n");
}

} // namespace

int main(int argc, char** argv) {
	std::string pipeName = "foo_librespot";
	std::vector<std::string> arguments;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--pipe" && i + 1 < argc) {
			pipeName = argv[++i];
		} else {
			arguments.push_back(arg);
		}
	}

	if (arguments.empty()) {
		usage();
		return 2;
	}

	const std::wstring widePipe(pipeName.begin(), pipeName.end());
	const std::string mode = arguments.front();

	try {
		if (mode == "spawn") {
			if (arguments.size() < 2) {
				usage();
				return 2;
			}
			const std::string directory = arguments[1];
			return run_spawn(std::wstring(directory.begin(), directory.end()), widePipe);
		}

		flp::bridge_client client(widePipe);

		if (mode == "status") return run_status(client);

		if (mode == "info") {
			if (arguments.size() < 2) {
				usage();
				return 2;
			}
			return run_info(client, std::vector<std::string>(arguments.begin() + 1, arguments.end()));
		}

		if (mode == "resolve") {
			if (arguments.size() < 2) {
				usage();
				return 2;
			}
			return run_resolve(client, arguments[1]);
		}

		if (mode == "play") {
			if (arguments.size() < 2) {
				usage();
				return 2;
			}
			const std::string uri = arguments[1];
			int startMs = 0;
			long long frameLimit = 0; // 0 = all
			long long expectStartFrame = -1;
			long long expectFrames = -1;
			unsigned abortAfterMs = 0;
			for (size_t i = 2; i < arguments.size(); ++i) {
				const std::string& arg = arguments[i];
				if (arg == "--start-ms" && i + 1 < arguments.size()) {
					startMs = std::atoi(arguments[++i].c_str());
				} else if (arg == "--frames" && i + 1 < arguments.size()) {
					const std::string value = arguments[++i];
					frameLimit = value == "all" ? 0 : std::atoll(value.c_str());
				} else if (arg == "--expect-start-frame" && i + 1 < arguments.size()) {
					expectStartFrame = std::atoll(arguments[++i].c_str());
				} else if (arg == "--expect-frames" && i + 1 < arguments.size()) {
					expectFrames = std::atoll(arguments[++i].c_str());
				} else if (arg == "--abort-after-ms" && i + 1 < arguments.size()) {
					abortAfterMs = static_cast<unsigned>(std::atoi(arguments[++i].c_str()));
				}
			}
			return run_play(client, uri, startMs, frameLimit, expectStartFrame, expectFrames,
				abortAfterMs);
		}

		usage();
		return 2;
	} catch (const flp::bridge_error& error) {
		std::printf("bridge error code=%s message=%s\n", error.code.c_str(), error.what());
		return 1;
	} catch (const flp::cancelled&) {
		std::printf("cancelled\n");
		return 3;
	} catch (const std::exception& error) {
		std::printf("error: %s\n", error.what());
		return 1;
	}
}
