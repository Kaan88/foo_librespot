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
  cmd/bridgectl/         bridgectl.exe (protocol client for manual testing)
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
librespot-bridge.exe [--pipe <name>] [--parent-pid <pid>] [--cache-mb <n>] [--cache-dir <dir>] [--debug]
librespot-bridge.exe login
```

- `--cache-mb` (default 0 = off) — size limit of the persistent audio cache in
  MiB; `--cache-dir` (default `%LOCALAPPDATA%\foo_librespot\cache`). See
  "Phase 5" below.

- `--pipe` (default `foo_librespot`) — named pipe, `\.\pipe\<name>`, byte mode,
  many concurrent connections, one goroutine per connection. The default pipe
  security descriptor restricts it to the current user, SYSTEM and
  Administrators.
- `--parent-pid` — wait on the process handle and shut down (exit 0) when it is
  signaled. The result is verified with `GetExitCodeProcess` and the exit code
  is logged; a parent that cannot be opened (access denied, still starting) is
  retried, never assumed dead. The process named here is also the only client
  the `shutdown` op accepts (checked with `GetNamedPipeClientProcessId`), so
  foobar2000 can stop its own bridge on quit but never another instance's.
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
the end of the track, then closes. Ops: `status`, `resolve`, `info`, `open`,
`library`, `search`, `artist`, `saved`/`save`/`unsave`, `playlist_add`,
`follow`/`unfollow`/`following`, `playlist_create`/`rename`/`delete`/`remove`,
`radio`, `config`. Errors carry `code` ∈
`not_logged_in | unavailable | bad_request | internal | unsupported` (plus the
v4 codes in `../proto/PROTOCOL.md`). See `../proto/PROTOCOL.md` for the exact
shapes.

`status` answers immediately from cached state (it never waits for the
session), so a client can always ask what is going on. A request that needs the
session waits for the in-flight creation — bounded by its own timeout — and
fails with `unavailable` and `connecting to Spotify failed: <reason>` when that
creation fails. A request that fails with a session/connection error is retried
once after the session is recreated; definitive answers (restricted or
unplayable track, refused context or collection, missing artist, bad input) are
returned as-is, and a feature this session cannot serve answers
`code: "unsupported"`.

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

`info` takes an optional `bitrate` (96/160/320) and describes the file `open`
would pick for it, so a client can show codec and bitrate before playing.

Metadata is cached in memory (LRU): the raw `TRACK_V4`/`AUDIO_FILES` metadata
and the built `track_info` for 20 000 URIs (per preferred bitrate), album and
artist items by URI, and artist pages for 5 minutes. A playlist opened twice
fetches nothing the second time; a 500-track `info` after its `resolve` takes
milliseconds.

### Resolve context

`resolve` also returns, for album and playlist contexts (and for Liked Songs),
the `context` object: `uri`, `name`, `owner`, `image_url`, `revision` and
`track_count`. For playlists the revision is the playlist service's revision
(`metadata["playlist.revision"]`, hex); Spotify-generated playlists report an
all-zero prefix but a real content revision. Albums and Liked Songs have no
service revision, so theirs is a truncated SHA-256 of the ordered track-URI
list, which changes exactly when the contents do.

### Library, search and artist

Everything below comes from the session's own APIs; no Web API token is
involved.

- `library` reads the playlist service's decorated rootlist
  (`/playlist/v2/user/<u>/rootlist`, one request) for the user's playlists and
  the Spotify-owned "made for you" playlists, and the collection service's
  `collection` set for Liked Songs and saved albums plus the `artist` set for
  followed artists. Album and artist names, covers and portraits come from
  batched `ALBUM_V4`/`ARTIST_V4` extended metadata. The five sources are
  fetched in parallel; the reply is cached for 30 s, so reopening the tab is
  instant.
- `recently_played` comes from Spotify's recently-played service
  (`/recently-played/v3/user/<u>/recently-played?format=json&...`), which lists
  contexts rather than tracks. Albums, artists and Liked Songs are described
  like their library sections; playlists get name, owner, picture and revision
  from the playlist service. `top_tracks` and `top_artists` are returned empty:
  no top-list source is reachable from this login (the TOPTracks/TOPARTISTS
  context upstream is not deployed, the profile view's top routes answer 404,
  and the Web API answers 429); see PROTOCOL.md for the evidence.
- `search` asks the context resolver for `spotify:search:<query>`, which is
  Spotify's internal search list: up to 20 tracks. Playlists come from
  Spotify's search service (`/searchview/km/v4/search/<query>`), which needs
  `platform=WebPlayer&clientVersion=...` in the query and answers with hits
  carrying URI, name, owner and image. Its albums and artists could be used
  too, but the reply keeps deriving those from the matching tracks. A search
  service failure only empties `playlists`; the tracks still answer.
- `artist` reads `ARTIST_V4` (top tracks for the account's country, album
  groups, single groups) and resolves the release names and covers with batched
  `ALBUM_V4`.

### Phase 4: saved, save/unsave, playlist_add, radio

All through spclient (the public Web API answers 429 for this login's token):

- `saved` — `POST /collection/v2/contains` (set `collection`, protobuf
  `ContainsRequest`/`ContainsResponse`).
- `save` / `unsave` — `POST /collection/v2/write` (set `collection`,
  `WriteRequest`, items with `added_at` or `is_removed`). Afterwards the cached
  library is dropped; the Liked Songs revision is a hash of the freshly read
  collection, so it changes right away.
- `playlist_add` — `GET /playlist/v2/playlist/<id>?decorate=revision` for the
  base revision, then `POST /playlist/v2/playlist/<id>/changes` with a
  playlist4 `ListChanges` holding one `ADD` op (`add_last`). The reply carries
  the revision read again afterwards. `dry_run` stops after building the
  request. The POST is never resent automatically (it is not idempotent), except
  for one retry on 409 with a fresh revision.
- `radio` — tried in order: context resolver on `spotify:station:<kind>:<id>`
  (this is what answers), `POST /context-resolve/v1/autoplay`, and
  `GET /radio-apollo/v3/stations/<seed uri>`. The first one that yields tracks
  wins and is logged (`radio for ...: N tracks via ...`).
- `library` playlist items carry `editable`; `track_info` carries `album_uri`
  and `artist_uris`.

### Phase 5: audio cache, error codes, album gain

**Audio cache.** `player.Options.Cache` takes a `cache.Cache` (go-librespot's
`cache` package): encrypted audio files by file id, LRU eviction at the size
limit, written atomically once a file has been fully downloaded (a track skipped
half way is not cached). The manager builds it from `--cache-mb` / `--cache-dir`,
and the `config` op can change it while the bridge runs: it makes a new
`player.Player` with the new cache and swaps it into the current session
generation (under the write lock every request holds for reading); the old
player is closed, streams already open keep decoding from what they hold,
nothing reconnects. `config` with `clear_cache` runs a GC first (a closed stream
keeps its cache file open until the Go finalizer runs, and Windows cannot delete
open files), deletes the files and rebuilds the cache so its size accounting
restarts from what is left. Whether an `open` came from the cache is detected
from the library's own "using cached audio file" debug line (counted by a logger
wrapper around the player's log; `NewStream` is serialised) and reported as
`from_cache` plus a log line `opened <uri> at <ms> ms from cache|network in <time>`.
`bridgectl config [--cache-mb N] [--clear-cache]` drives it.

**Errors.** `spotifyx.Classify` maps library errors to protocol codes (table in
`PROTOCOL.md`): `ap.AccesspointLoginError` (`PremiumAccountRequired`,
`BadCredentials`, ...), `librespot.ErrMediaRestricted`,
`librespot.ErrNoSupportedFormats`, `librespot.HTTPStatusError` /
`spclient.StatusError` (429, 404/410, 401/403), `spclient.RateLimitedError`
(carries `Retry-After`), `audio.KeyProviderError`, and the extended-metadata
batch entries whose status only exists as text
(`extended metadata request returned status N`). The account's product type is
read from the accesspoint's product-info packet and only refines a failure that
already happened (a free account's refused key is `premium_required`, a Premium
account's is `track_unavailable`). `Manager.Do` returns a `*CodedError` (the
original error stays reachable through `Unwrap`, so the permanent/retry
decisions keep working); a rate limit is permanent, it does not rebuild the
session.

**Album gain.** Already in `BuildTrackInfo`: `default_album_normalization_params`
of the AUDIO_FILES response, same formula as the track gain (see PROTOCOL.md).
With `--debug`, an `open` also logs the ogg header's own gains next to the
AUDIO_FILES-derived ones (header read through reflection, logging only).

### Following and playlist management

All writes go through spclient and are never resent blindly (a 409 is the only
retry, once, with a fresh revision):

- `follow` / `unfollow` accept artist, album and playlist URIs. Artists use the
  collection `artist` set, albums the collection `collection` set, playlists
  the rootlist (`POST /playlist/v2/user/<u>/rootlist/changes`, ADD `add_first`
  at the top / REM at the entry's index). `following` answers collection
  contains for artists and albums and rootlist membership for playlists.
- `playlist_create` posts a playlist4 `ListUpdateRequest` (name) to
  `/playlist/v2/playlist` and adds the new playlist to the top of the rootlist.
- `playlist_rename` sends an `UPDATE_LIST_ATTRIBUTES` change;
  `playlist_delete` removes the entry from the rootlist (Spotify has no
  delete).
- `playlist_remove` validates every `{uri,index}` against the current contents
  and sends one REM per index, highest first, so a moved playlist answers
  `bad_request` instead of removing the wrong track.
- Every write op has `dry_run`, which returns the request it would send.
- `search` playlists come from `/searchview/km/v4/search/<query>` with
  `platform=WebPlayer&clientVersion=...` (without a client version the service
  answers 400).

### v6: shutdown, logout, liked, report_play

- `status` reports `pid` and `parent_pid`, so the component can tell its own
  bridge from another instance's or a leftover one.
- A bridge started before the login picks up `credentials.json` within a
  second, so logging in from the preferences needs no restart; `logout` drops
  the session and deletes the file.
- `liked` lists the Liked Songs track URIs (for `%spotify_liked%`).
- `report_play` sends one `RawCoreStream` event to
  `/gabo-receiver-service/v3/events/`, built like the desktop client's
  (go-librespot ships only a dummy event manager). Sent once, never retried;
  `dry_run` returns it instead. It has not been verified against Spotify live.
- `config` takes `debug` to switch debug logging at runtime.

### Item images

Covers and portraits are the largest `i.scdn.co` image of the metadata. A
rootlist playlist uses its `image_url` format attribute when Spotify provides
one (generated/editorial playlists) and otherwise its own picture
(`u.scdn.co/images/pl/default/<hex>`); a user playlist without either has
`"image_url": null`.

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
bridgectl [--pipe <name>] info <uri-or-url>... [--bitrate 96|160|320]
bridgectl [--pipe <name>] library
bridgectl [--pipe <name>] search <query> [--limit N]
bridgectl [--pipe <name>] artist <uri-or-url>
bridgectl [--pipe <name>] saved|save|unsave <track-uri>...
bridgectl [--pipe <name>] playlist-add <playlist-uri> <track-uri>... [--dry-run]
bridgectl [--pipe <name>] follow|unfollow <artist|album|playlist uri>... [--dry-run]
bridgectl [--pipe <name>] following <artist|album|playlist uri>...
bridgectl [--pipe <name>] playlist-create <name> [--dry-run]
bridgectl [--pipe <name>] playlist-rename <playlist-uri> <name> [--dry-run]
bridgectl [--pipe <name>] playlist-delete <playlist-uri> [--dry-run]
bridgectl [--pipe <name>] playlist-remove <playlist-uri> <index>:<track-uri>... [--dry-run]
bridgectl [--pipe <name>] radio <track|artist|album|playlist uri>
bridgectl [--pipe <name>] config [--cache-mb N] [--clear-cache] [--debug on|off]
bridgectl [--pipe <name>] liked
bridgectl [--pipe <name>] report-play <track-uri> --started-at MS --ms-played MS --dry-run
bridgectl [--pipe <name>] logout
bridgectl [--pipe <name>] shutdown
bridgectl [--pipe <name>] open-to-wav <uri> <out.wav>
          [--start-ms N] [--seconds S] [--bitrate 96|160|320]
```

`open-to-wav` prints the header (including `start_frame`) and the track info,
then writes the PCM as 16-bit WAV and reports frames, wall time and peak/RMS.
`report-play` refuses to run without `--dry-run`.

## Phase 0 spike

`bin\spike.exe` keeps the original proof-of-concept commands and now shares the
session/metadata/seek code with the bridge:

```text
spike.exe login
spike.exe dump <spotify:track:ID | open.spotify.com/track/ID | ID> <out.wav>
              [--start-ms N] [--seconds S] [--bitrate 96|160|320] [-v]
```

## Live verification

Against a logged-in Premium account (`%APPDATA%\foo_librespot\credentials.json`):

- `status` → `{"ok":true,"logged_in":true,"username":"<your-username>","country":"<CC>","version":"0.2.0"}`.
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

Phase 3, measured on the same account:

- `library` → liked 1, made for you 8, playlists 25, albums 28, artists 5 in
  520–543 ms cold (four sources in parallel plus two metadata batches), 16 ms
  from the 30 s cache.
- `search "nation of language" --limit 5` → 5 tracks, 5 albums, 5 artists, 0
  playlists; the top track is Nation of Language's "Weak In Your Light"
  (`vorbis 320`). With limit 20: 20 tracks, 16 albums, 11 artists in ~800 ms.
- `artist spotify:artist:5JWBow4ywgKNQ5HBxY8hcz` → Nation of Language, 10 top
  tracks, 4 albums, 34 singles in 550–570 ms; 13 ms from the page cache.
- `resolve https://open.spotify.com/playlist/37i9dQZF1E8M5Z6GDxnHFg` → 50
  tracks, context "On Division St Radio" / owner `spotify` / a pickasso image /
  revision `00000000c276921a…` / count 50; a second resolve returned the same
  revision. A 972-item playlist resolved to 961 tracks in ~3.1 s, and `info`
  for 500 of them then took 29 ms.
- `info spotify:track:4odb2UIgc0u22MNV7p4iQq --bitrate 320` → `vorbis` /
  `320`; with `--bitrate 96` → `vorbis` / `96`.

v5 additions, measured on the same account:

- `library` → liked 1, made for you 8, playlists 25, albums 28, artists 5,
  recently played 2, top tracks 0, top artists 0 in 900 ms cold (from the
  recently-played service plus two playlist metadata reads); from the 30 s
  cache 0 ms. The recently-played items (an album and a playlist) came with
  cover, and the playlist with its revision.
- `search "nation of language" --limit 5` → 5 tracks, 5 albums, 5 artists,
  5 playlists in 1.26 s; the playlists are Nation of Language Mix/Radio,
  This Is Nation of Language and two user setlists.
- `following` with a followed artist, a saved album, a followed playlist,
  Today's Top Hits, an unsaved album and an unfollowed artist →
  `[true,true,true,false,false,false]` in 537 ms.
- `--dry-run` of follow (collection `artist`/`collection` writes plus a
  rootlist ADD `add_first`), unfollow (`is_removed` plus a REM at index 2),
  playlist-create (`ListUpdateRequest`), playlist-rename
  (`UPDATE_LIST_ATTRIBUTES`) and playlist-remove (REM 1 then REM 0) returned
  the exact requests in 83–166 ms. A wrong `{uri,index}` answered
  `bad_request` naming the track that is actually there, and nothing was sent.
- No real write was issued during verification; a follow dry run left
  `following` unchanged.

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
