# foo_librespot bridge

Windows audio bridge for the future foobar2000 Spotify plugin. It uses
[`github.com/devgianlu/go-librespot`](https://github.com/devgianlu/go-librespot)
as a **library**: one shared Spotify session, created eagerly in the background,
decodes tracks to interleaved 44100 Hz stereo float32 PCM and serves them over a
named pipe, with no Spotify Connect state machine and no audio output device.

The wire contract lives in [`../proto/PROTOCOL.md`](../proto/PROTOCOL.md) and is
implemented by `bin/librespot-bridge.exe`.

```
bridge/
  cmd/bridge/            librespot-bridge.exe (pipe server, `login`)
  cmd/bridgectl/         bridgectl.exe (status/resolve/info/open-to-wav client)
  cmd/spike/             Phase 0 tool: `login`, `dump <track> <out.wav>`
  internal/bridge/       protocol types, named-pipe server, streaming loop
  internal/spotifyx/     shared session, country, metadata, resolve, sample-exact open
  internal/seek/         frame math, page-boundary anchoring, frame discard
  internal/spotifyuri/   URI/URL parsing
  internal/creds/        %APPDATA%\foo_librespot\credentials.json
  internal/logging/      go-librespot logger adapter + rotating log file
  internal/wav/          streaming 16-bit WAV writer
  internal/spike/        Phase 0 dump loop (now on top of internal/spotifyx)
  build.ps1              builds all three exes with cgo + MinGW libraries
```

## Build

```powershell
cd bridge
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

`build.ps1` prepares `PATH` (`C:\Program Files\Go\bin`, `C:\msys64\mingw64\bin`),
`CGO_ENABLED=1`, `CC=gcc`, `PKG_CONFIG_PATH` and the two cgo flags below, then
builds:

| output | package |
| --- | --- |
| `bin\librespot-bridge.exe` | `./cmd/bridge` |
| `bin\spike.exe` | `./cmd/spike` |
| `bin\bridgectl.exe` | `./cmd/bridgectl` |

The C decode libraries (libogg, libvorbis, libFLAC, mpg123) are linked
**statically**, so the exes need no MinGW DLLs at runtime:

- `CGO_CFLAGS=-DFLAC__NO_DLL` — FLAC's headers otherwise declare `dllimport`,
  and linking `libFLAC.a` then fails on `__imp_FLAC__*`.
- `CGO_LDFLAGS=-lshlwapi` — mpg123's static archives call `PathIsRelativeW` and
  friends.
- `-ldflags "-linkmode external -extldflags -static"` — MSYS2's `libogg-0.dll`
  does not export `ogg_stream_iovecin`, which `xlab/vorbis-go` references;
  `libogg.a` does.

Checks (tests need the same link flags because they import the decoders):

```powershell
go vet ./...
go test -ldflags "-linkmode external -extldflags -static" ./...
```

## Running the bridge

```text
librespot-bridge.exe [--pipe <name>] [--parent-pid <pid>] [--debug]
librespot-bridge.exe login
```

- `--pipe` (default `foo_librespot`) — named pipe, `\.\pipe\<name>`, byte mode,
  many concurrent connections, one goroutine per connection. The default pipe
  security descriptor restricts it to the current user, SYSTEM and
  Administrators.
- `--parent-pid` — wait on the process handle and shut down (exit 0) when it is
  signaled. The result is verified with `GetExitCodeProcess` and the exit code
  is logged; a parent that cannot be opened (access denied, still starting) is
  retried, never assumed dead.
- Single instance — if the pipe is connectable (`winio.DialPipe`), a second
  bridge exits 0 immediately. No status round trip is involved.
- Logs go to `%APPDATA%\foo_librespot\bridge.log` (append, rotated once to
  `bridge.log.1` past 5 MiB). Every line starts with a timestamp and
  `[pid=<pid>]`, because several bridges append to the same file.
- `login` runs the OAuth device-authorization flow on the console and writes
  `%APPDATA%\foo_librespot\credentials.json`; the bridge and the spike share it.

### Protocol

One request per connection: one JSON line in, one JSON line out. `open` keeps
the connection and follows the header with raw little-endian float32 PCM until
the end of the track, then closes. Ops: `status`, `resolve`, `info`, `open`.
Errors carry `code` ∈ `not_logged_in | unavailable | bad_request | internal`.
See `../proto/PROTOCOL.md` for the exact shapes.

`status` answers immediately from cached state (it never waits for the
session), so a client can always ask what is going on. A request that needs the
session waits for the in-flight creation — bounded by its own timeout — and
fails with `unavailable` and `connecting to Spotify failed: <reason>` when that
creation fails. A request that fails with a session/connection error is retried
once after the session is recreated; definitive answers (restricted or
unplayable track, refused context, bad input) are returned as-is.

### The shared session

`internal/spotifyx.Manager` starts creating the session as soon as the bridge
starts (when credentials exist) and owns it for all connections:

- creation runs on the manager's own context, never a request's: a client that
  gives up cannot cancel or poison it; concurrent requests share one in-flight
  creation (single-flight) and a failed round is retried with backoff;
- each accesspoint attempt has a 10 s deadline and the next accesspoint is
  tried right away, so one dead port (for example a blocked 4070) costs 10 s
  instead of the library's 30 s handshake timeout. Firewall-friendly ports
  (443, 80) are preferred over 4070;
- `session.NewSessionFromOptions` with the stored credentials;
- one `player.NewPlayer` used only as a stream factory (`SetPrimaryStream` is
  never called, so no audio device is opened);
- the account **country**, taken from the accesspoint's
  `PacketTypeCountryCode` packet exactly like go-librespot's daemon
  (`sess.Accesspoint().Receive(...)`), kept in an atomic and exposed to the
  player's `CountryCode` callback. A real country re-enables go-librespot's
  media-restriction check and its alternative-track relinking, which is what
  makes tracks such as `spotify:track:4uLU6hMCjMI75M1A2tKUQC` play: Spotify
  relinks it to an alternative, and `track_info.uri` still reports the URI that
  was **requested**, while title/artists/audio come from what actually played.

`player.NewStream` is serialised (its CDN quarantine map is not concurrency
safe); decoding and streaming afterwards are fully concurrent per connection.

### Metadata

`resolve` and `info` use the extended-metadata API in batches of 100 entities
(up to 4 in flight, so 500 URIs is five requests) and ask for both
`TRACK_V4` and `AUDIO_FILES` per entity. From that:

- title, artists, album, album artist, track/disc number, year, duration;
- `cover_url` = `https://i.scdn.co/image/<hex>` for the largest album image
  (`album.cover_group.image`, falling back to `album.cover`);
- codec/bitrate of the format `open` would select for the preferred bitrate;
- ReplayGain 2.0 values from Spotify's loudness metadata
  (`track_gain_db = -18 - loudness_db`, `track_peak = 10^(true_peak_db/20)`,
  same for the album values when present). Not applied to the audio.

`resolve` expands album, playlist and `spotify:user:<u>:collection` (Liked
Songs, newest first, using the session's own account) contexts. Playlists are
read through the playlist service (like go-librespot's own playback), which
lists every item — including Spotify's generated playlists such as Daily Mixes
— instead of relying on context resolve; the pages after the first are fetched
as playback gets there. `info` preserves the request order; an unknown track
yields an unplayable entry with empty fields.

### Sample-accurate seeking

foobar2000 seeks by closing the connection and sending `open` with a new
`start_ms`; the header's `start_frame` is `round(start_ms * 44100 / 1000)` and
the first PCM frame sent is exactly that frame.

librespot seeks in milliseconds and lands on a codec boundary; for Vorbis the
decoder starts up to ~140 ms before (or slightly after) the request. The bridge:

1. opens the decoder `600 ms` before the target (`0` when the target is under
   one second), so the landing point is almost always before it;
2. pins the decoder's *exact* position: `Source.PositionMs()` only truncates to
   milliseconds, but the Vorbis decoder keeps the Ogg granule position (the end
   of the last decoded page) in `lastGranulepos`. `internal/spotifyx` reads it
   reflectively; the first granule after a seek can be stale, so the bridge
   reads one frame at a time until the granule changes twice and anchors at the
   page boundary (exact to the sample);
3. discards decoded frames until the anchored position reaches `start_frame`;
4. reopens further back if a seek lands after the target, and falls back to
   decoding from frame 0 (exact, just slower) if a decoder exposes no granule
   or the discards hit the end of the track.

For decoders whose seek is already sample-accurate (mp3, flac) the anchor is
`floor(seek_ms * 44100 / 1000)`, matching librespot's own `SetPositionMs`
arithmetic. A `start_ms` at or past the track duration returns a valid header
and no audio.

`internal/seek` holds the frame math and the anchoring loop, with unit tests for
the rounding and the stale-granule handling.

## bridgectl

```text
bridgectl [--pipe <name>] status
bridgectl [--pipe <name>] resolve <uri-or-url>
bridgectl [--pipe <name>] info <uri-or-url>...
bridgectl [--pipe <name>] open-to-wav <uri> <out.wav>
          [--start-ms N] [--seconds S] [--bitrate 96|160|320]
```

`open-to-wav` prints the header (including `start_frame`) and the track info,
then writes the PCM as 16-bit WAV and reports frames, wall time and peak/RMS.

## Phase 0 spike

`bin\spike.exe` keeps the original proof-of-concept commands and now shares the
session/metadata/seek code with the bridge:

```text
spike.exe login
spike.exe dump <spotify:track:ID | open.spotify.com/track/ID | ID> <out.wav>
              [--start-ms N] [--seconds S] [--bitrate 96|160|320] [-v]
```

## Live verification

Against the logged-in Premium account (`%APPDATA%\foo_librespot\credentials.json`):

- `status` → `{"ok":true,"logged_in":true,"username":"testuser0001","country":"SE","version":"0.1.0"}`.
- `resolve https://open.spotify.com/album/4yP0hdKOZPNshxUOjY0cZj` → 14 tracks with
  covers and gains; Liked Songs → 377 tracks; `info` with all 377 URIs in one
  request (4 batches) returned 377 entries in order, all with `cover_url` and
  gains.
- `open spotify:track:0VjIjW4GlUZAMYd2vXMi3b` at 60000 ms: header
  `start_frame 2646000`, 5 s of non-silent PCM (peak 0.9855, RMS 0.3333).
  Comparing those samples with a decode from frame 0: **0 of 882000 bytes
  differ at frame 2646000**, and every ±1-frame alignment differs (a 1-frame
  shift changes 440868 bytes). The same check at 180000 ms (frame 7938000) is
  also byte-identical.
- `open spotify:track:4uLU6hMCjMI75M1A2tKUQC` (the track that used to fail with
  `failed retrieving aes key with code 1`) plays: `uri` is the requested
  `4uLU…`, title/artists are the relinked alternative Spotify served, and the
  audio is non-silent.
- A second instance on the same pipe exits 0; `--parent-pid` exits 0 when the
  parent dies; a bridge with no credentials file answers
  `{"ok":false,"code":"not_logged_in"}`; clients closing mid-stream stop the
  bridge's decoder promptly; 5 concurrent `info` requests succeed while a
  stream is being written.

## Notes and limits

- The exact Vorbis anchor reads `lastGranulepos` through reflection because the
  library exposes only millisecond positions. If a future go-librespot renames
  that field, the bridge detects it (`Granule` reports `ok == false`) and falls
  back to decoding from frame 0, which is exact but slower for large seeks.
- Vorbis and MP3 are the formats Spotify serves to this client; FLAC (PlayPlay)
  is not enabled, but its seek path is handled.
- Liked Songs always resolves the *session account's* collection, whatever user
  the URI names (the protocol has no notion of another user's library).
- Gains are metadata only; the PCM is never gain-adjusted, so foobar2000's own
  ReplayGain handling can apply them.
