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
Errors: `{"ok":false,"error":"<message>","code":"<not_logged_in|unavailable|bad_request|internal>"}`; v4 adds `premium_required`, `rate_limited`, `region_unavailable` and `track_unavailable` (see "v4 additions").
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

## v2 additions (Phase 3: Spotify tab and synced playlists)

All v1 requests stay as they are. New fields are optional and clients must ignore unknown fields.

### Changes to v1 requests
- `info` takes an optional `"bitrate":96|160|320` (the client's preferred setting). Every returned `<track_info>` then has `bitrate` set to the bitrate of the file `open` would pick (not null), and `codec` set likewise.
- `resolve` replies also carry, for album and playlist contexts:
  `"context":{"uri":"spotify:playlist:ID","name":"Daily Mix 1","owner":"Spotify","image_url":"https://...","revision":"<opaque string>","track_count":50}`.
  `revision` changes whenever the playlist's contents change (playlist revision; for Liked Songs and albums, a hash of the track URI list). Clients use it to skip syncs when nothing has changed.

### library  (contents of the Spotify tab)
`{"op":"library"}` ->
```
{"ok":true,"sections":[
  {"id":"liked","title":"Liked Songs","items":[<item>]},          // one collection item
  {"id":"made_for_you","title":"Made for you","items":[<item>...]},
  {"id":"playlists","title":"Your playlists","items":[<item>...]},
  {"id":"albums","title":"Albums","items":[<item>...]},
  {"id":"artists","title":"Artists","items":[<item>...]}]}
```
`<item>`: `{"uri":"spotify:playlist:ID","kind":"playlist|album|artist|collection","name":"...","subtitle":"owner or artist","image_url":"https://...|null","track_count":50|null,"revision":"...|null"}`
- Liked Songs is `spotify:user:<username>:collection`, kind `collection`.
- "Made for you" holds Spotify-generated playlists the user follows or that are in their rootlist (Daily Mixes, Discover Weekly, Release Radar, owner "Spotify"). If a separate "made for you" source isn't reachable through go-librespot, put Spotify-owned playlists from the rootlist here.
- An empty section is returned with `items: []`, never left out.

### search
`{"op":"search","query":"nation of language","limit":20}` ->
`{"ok":true,"tracks":[<track_info>...],"albums":[<item>...],"artists":[<item>...],"playlists":[<item>...]}`
- `limit` applies to each category (default 20, max 50).

### artist  (artist page)
`{"op":"artist","uri":"spotify:artist:ID"}` ->
`{"ok":true,"artist":<item>,"top_tracks":[<track_info>...],"albums":[<item>...],"singles":[<item>...]}`

### Errors
New requests use the same error codes. A feature that go-librespot can't reach answers `{"ok":false,"code":"unsupported","error":"..."}`, and the client shows the message.

## v3 additions (Phase 4: context menu and write actions)

### saved  (is a track in Liked Songs?)
`{"op":"saved","uris":["spotify:track:ID",...]}` -> `{"ok":true,"saved":[true,false,...]}` (same order)

### save / unsave  (Liked Songs)
`{"op":"save","uris":["spotify:track:ID",...]}` -> `{"ok":true}`
`{"op":"unsave","uris":[...]}` -> `{"ok":true}`
- Afterwards the Liked Songs revision changes, so a sync picks it up.

### track_info additions (optional fields)
`<track_info>` gains two optional fields, present whenever the track's metadata is known (omitted for unknown tracks):
- `"album_uri":"spotify:album:ID"`
- `"artist_uris":["spotify:artist:ID", ...]` (one per credited artist that has an id, in the order of `artists`)
Clients use them for "Open album" / "Open artist".

### playlist_add  (add tracks to one of the user's Spotify playlists)
`{"op":"playlist_add","playlist":"spotify:playlist:ID","uris":["spotify:track:ID",...]}` -> `{"ok":true,"revision":"..."}`
- Appends at the end. Fails with `bad_request` if the user can't edit that playlist.
- `library` items for playlists get an optional `"editable":true|false` so clients can list only playlists they can add to. It is true when the user owns the playlist, it is collaborative, or the playlist service's capabilities say `can_edit_items`.
- Optional request field `"dry_run":true`: the bridge reads the playlist's current revision, builds the change and returns it without sending: `{"ok":true,"revision":"<base revision>","dry_run":true,"request":{"method":"POST","path":"/playlist/v2/playlist/ID/changes","body_text":"<ListChanges, protobuf text>","body_len":98}}`.
- A 403/404 from Spotify (not allowed to edit, or no such playlist) answers `bad_request`; any other refusal answers `unsupported` with Spotify's message. A 409 (playlist changed meanwhile) is retried once with the fresh revision.

### radio  (tracks for a radio based on a track, artist, album or playlist)
`{"op":"radio","uri":"spotify:track:ID"}` -> same reply shape as `resolve` (with `context`: name like "<title> Radio", uri of the radio/station context).
- Seeds: track, artist, album or playlist. `context.uri` is `spotify:station:<kind>:<id>`, `context.owner` is `Spotify`, `revision` is a hash of the returned track list (a radio changes on every call). Usually 50 tracks; for a track seed the seed itself is first.

### Errors
If Spotify rejects a write from this kind of login, answer `{"ok":false,"code":"unsupported","error":"<what Spotify said>"}`, never a fake success.

## v4 additions (Phase 5: audio cache, specific errors, album ReplayGain)

All earlier requests and fields stay as they are; everything below is additive. A v3 client keeps working against a v4 bridge, and a v4 client must treat an unknown-op error from `config` as "old bridge".

### Audio cache (`--cache-mb` flag and `config` op)

go-librespot's audio cache stores the **encrypted** audio files as the CDN serves them (the audio key is still fetched for every playback, so a cached file is useless without a valid login), keyed by Spotify file id, with least-recently-used eviction once the size limit is reached. The bridge keeps it in `%LOCALAPPDATA%\foo_librespot\cache\audio` (override: `--cache-dir <dir>`; the library adds the `audio` subfolder).

- Spawn flag: `librespot-bridge.exe ... --cache-mb <N>`: limit in MiB, `0` = off. When the flag is missing the cache is **off**, so old components never create one. The component passes its setting on every spawn.
- A file is written once it has been downloaded completely (a track that is skipped half way is not cached). The first open of a track is unchanged; a later open reads the file from disk and skips the CDN download and the storage-resolve round trip.
- `open` headers gain an optional `"from_cache":true` (omitted when the audio came from the network).

`config` (change settings of a running bridge: no restart, no reconnect):

`{"op":"config","cache_mb":1024,"clear_cache":true}` -> `{"ok":true,"cache_mb":1024,"cache_dir":"C:\\Users\\...\\foo_librespot\\cache","cache_bytes":9177336}`

- `cache_mb` (optional, int 0..1048576): sets the limit; `0` turns the cache off. Takes effect for the next `open`; streams that are already running are not touched. The bridge replaces its stream factory in place; the Spotify session stays up.
- `clear_cache` (optional, bool): deletes every cached file (done after `cache_mb` when both are given). Files of a stream that is playing right now cannot be deleted on Windows; the reply is then `internal` with a message saying so, and everything else is deleted anyway.
- With neither field the request only reports the state. `cache_bytes` is the size of the cached audio on disk.
- `bad_request` for a `cache_mb` out of range. A bridge older than v4 answers `bad_request` ("unknown op").

`status` gains two optional fields: `"product":"premium"` (account product type from the accesspoint's product-info packet; absent until known) and `"last_error_code":"premium_required"` (the error code that classifies `last_error`, when one applies).

### Error codes

The `code` of an error reply can now also be one of:

| code | meaning | extra |
| --- | --- | --- |
| `premium_required` | The account is not Premium, or Premium has expired. Sources: the accesspoint refusing the login with `PremiumAccountRequired`; or, for an account whose product type (product-info packet) is not `premium`, a refused audio key, a missing audio file or a 401/403 from the CDN. | |
| `rate_limited` | Spotify answered HTTP 429 (from any endpoint). | optional `"retry_after":<seconds>` in the reply when Spotify said so (a connect-state 429 carries the `Retry-After` header; the audio and metadata endpoints do not, then the field is absent). |
| `region_unavailable` | The track is restricted in the account's country and relinking found no playable alternative (`media is restricted`). | |
| `track_unavailable` | The track is removed or has no playable audio file: metadata 404/410, no supported format, a refused audio key or a 403 for a Premium account. | |
| `not_logged_in` | As before, and now also when Spotify rejects the saved login (`BadCredentials`, `CouldNotValidateCredentials`). | |

`unavailable` stays for a session that cannot be established (`connecting to Spotify failed: <reason>`) and for anything else; `internal` for the rest. The `error` text is a message fit for showing to the user. Any request can return these codes (`open` and `info` most often). A client that does not know a code must handle it like `unavailable`.

### Album ReplayGain

`<track_info>.album_gain_db` / `album_peak` were defined in v1 and are now documented as filled: Spotify's AUDIO_FILES extended metadata carries `default_album_normalization_params` next to `default_file_normalization_params`, both `{loudness_db, true_peak_db}`. The same formula applies to both, relative to the ReplayGain 2.0 reference of -18 LUFS:

```
track_gain_db = -18 - default_file_normalization_params.loudness_db
track_peak    = 10 ^ (default_file_normalization_params.true_peak_db / 20)
album_gain_db = -18 - default_album_normalization_params.loudness_db
album_peak    = 10 ^ (default_album_normalization_params.true_peak_db / 20)
```

Album values are `null` only when Spotify sends no album params for the file. They are present in `info`, `resolve` and in the `open` header's `track` (which describes the file that actually plays, and can differ from `info` after relinking). Clients should use the `open` header's values for ReplayGain during playback.

The ogg file header carries its own track/album gain and peak, but with a reference level 1 dB higher than the AUDIO_FILES-derived values above, and for singles it can repeat the track values as the album values. AUDIO_FILES is the source used.
