# foo_librespot bridge protocol (v1)

Process: `librespot-bridge.exe --parent-pid <pid> --pipe <name>`
- Started hidden by the foobar2000 component on first use. Exits when the parent pid dies.
- Logs to %APPDATA%\foo_librespot\bridge.log. Credentials: %APPDATA%\foo_librespot\credentials.json (same format as Phase 0 spike).
- Every log line carries a timestamp and the writing bridge's pid, because several bridges append to the same file.
- `librespot-bridge.exe login` runs the device-auth login interactively (console), same as `spike login`.
- One Spotify session is shared by all connections. It is created eagerly in the
  background as soon as the bridge starts (when credentials exist), retried with
  backoff, and replaced when it turns out to be dead. Session creation always
  runs on the bridge's own context: a client that gives up never cancels it.
  Concurrent requests share one in-flight creation.
- A second bridge started on a pipe name that a server already accepts on exits
  with status 0; the test is pipe connectability, not a request round trip.
  Transport: Windows named pipe `\.\pipe\<name>` (default name `foo_librespot`), byte mode, many concurrent instances.
Each request is ONE connection: client writes one JSON line (UTF-8, `\n`-terminated), server answers with one JSON line.
Errors: `{"ok":false,"error":"<message>","code":"<not_logged_in|unavailable|bad_request|internal>"}`.
- `not_logged_in`: no credentials file; the user has to run login.
- `unavailable`: the Spotify session is not usable (connecting failed, track
  restricted, unplayable format). A failed creation reports
  `"connecting to Spotify failed: <reason>"`; the bridge keeps retrying in the
  background.

## Requests

### status
`{"op":"status"}` ->
`{"ok":true,"logged_in":true,"username":"...","country":"SE","state":"connected","last_error":"","version":"0.1.0"}`

Answers from cached state and never waits for the session, so it returns in
milliseconds even while Spotify is still being reached.

- `logged_in` (bool): a credentials file exists. This, not the session state,
  decides whether the user has to log in.
- `username` (string): from the credentials; known while connecting. Empty when
  not logged in.
- `country` (string): account country from the accesspoint. Empty until a
  session exists.
- `state` (string): `"connecting"` (no usable session yet, created or being
  recreated), `"connected"` (a session is available) or `"error"` (the last
  creation failed; `last_error` says why, a retry is scheduled).
- `last_error` (string): reason of the last failed creation, else empty.
- `version` (string): bridge version.

### resolve  (expand a URI/URL into tracks; used to add items to playlists)
`{"op":"resolve","uri":"<spotify:track|album|playlist:ID or open.spotify.com/... url>"}`
-> `{"ok":true,"tracks":[ <track_info>, ... ]}`
- Playlists (including Spotify-generated ones, such as Daily Mixes) are read
  from the playlist service, so every item is listed; albums, tracks and Liked
  Songs use the context resolver. The reply can be large (up to 10000 tracks).
- Requests that need the session wait for an in-flight creation (bounded by the
  client's own timeout) and fail with `unavailable` when it fails.

### info
`{"op":"info","uris":["spotify:track:ID", ...]}` -> `{"ok":true,"tracks":[ <track_info>, ... ]}` (same order)

### open  (stream decoded audio)
`{"op":"open","uri":"spotify:track:ID","start_ms":0,"bitrate":320}`
-> header line `{"ok":true,"track":<track_info>,"sample_rate":44100,"channels":2,"start_frame":<int>}`
then the SAME connection carries raw interleaved float32 little-endian PCM until end of track, then the server closes the pipe.
- Audio is sample-accurate: the first frame sent is exactly `start_frame = round(start_ms*44100/1000)`
  (the bridge discards decoder overshoot after seeking).
- Seek = client closes the connection and sends a new `open` with the new `start_ms`.
- Client closing early must be handled cleanly (stop decoding, free stream).

`<track_info>`:
```
{"uri":"spotify:track:ID","title":"...","artists":["..."],"album":"...","album_artist":"...",
 "track_number":1,"disc_number":1,"date":"2020","duration_ms":200040,
 "codec":"vorbis","bitrate":320,"cover_url":"https://...",
 "track_gain_db":-5.64,"track_peak":1.104,"album_gain_db":null,"album_peak":null,
 "playable":true}
```
Gains are ReplayGain 2.0 values computed from Spotify loudness metadata: `track_gain_db = -18 - loudness_db`, `track_peak = 10^(true_peak_db/20)` (linear). Album values null if unknown. Not applied to the audio; foobar2000 applies them via its ReplayGain setting.
