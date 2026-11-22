# foo_librespot bridge protocol (v1)

Process: `librespot-bridge.exe --parent-pid <pid> --pipe <name>`
- Started hidden by the foobar2000 component on first use. Exits when the parent pid dies.
- Logs to %APPDATA%\foo_librespot\bridge.log. Credentials: %APPDATA%\foo_librespot\credentials.json (same format as Phase 0 spike).
- `librespot-bridge.exe login` runs the device-auth login interactively (console), same as `spike login`.
- One Spotify session is shared by all connections; it is created lazily and recreated on failure.

Transport: Windows named pipe `\.\pipe\<name>` (default name `foo_librespot`), byte mode, many concurrent instances.
Each request is ONE connection: client writes one JSON line (UTF-8, `\n`-terminated), server answers with one JSON line.
Errors: `{"ok":false,"error":"<message>","code":"<not_logged_in|unavailable|bad_request|internal>"}`.

## Requests

### status
`{"op":"status"}` -> `{"ok":true,"logged_in":true,"username":"...","country":"SE","version":"0.1.0"}`

### resolve  (expand a URI/URL into tracks; used to add items to playlists)
`{"op":"resolve","uri":"<spotify:track|album|playlist:ID or open.spotify.com/... url>"}`
-> `{"ok":true,"tracks":[ <track_info>, ... ]}`

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
Gains are ReplayGain-style: `gain_db = -18 - loudness_lufs`?? NO: use Spotify's reference: `gain_db = -14 - loudness_db`, peak = 10^(true_peak_db/20). Not applied to audio.
