# foo_librespot (foobar2000 component)

foobar2000 v2 component that plays Spotify tracks by talking to the
`librespot-bridge.exe` helper over the named-pipe protocol in
[`../proto/PROTOCOL.md`](../proto/PROTOCOL.md).

- Input: `spotify:track:<22-char id>` (44.1 kHz stereo float32 PCM from the bridge)
- **Spotify** Default UI element: a search box and a library tree (Liked Songs,
  saved albums, followed artists, made-for-you, your playlists and Recently
  played); click an entry to open it as a foobar2000 playlist, double-click to
  also start playing, right-click for follow/unfollow, playlist management and
  the playback queue
- Synced playlists named `Spotify · <name>`, tracked by the Spotify context URI
  and updated with a minimal diff on open, on a timer and on demand
- File > **Add Spotify link...**: resolves track / album / playlist URIs and
  `open.spotify.com` URLs into playlist entries
- Album art: front cover downloaded from the bridge's `cover_url` via
  foobar2000's `http_client`
- Preferences > Tools > **Spotify**: bridge status, Log in..., bitrate,
  sync interval and playlist name prefix

## Spotify tab

**Add New UI Element...** lists it as **Spotify** (Media Library viewers), so
it can be dropped into a Default UI tab stack next to Playlists / Properties /
ReFacets. In Columns UI it is listed under **Panels > Spotify**
(`src/spotify_tab_cui.cpp`, a `uie::window`). Both hosts wrap the same panel
code (`create_spotify_panel()` in `src/spotify_tab.cpp`); the host only
supplies colours, fonts and the dark mode flag. It is a plain Win32 TreeView
(comctl32); no ATL/WTL is required.

- **Your library**: Liked Songs, Albums (saved), Artists (followed; expand an
  artist to load its albums and singles through the bridge `artist` op).
- **Recently played** right below Your library; it is hidden when the bridge
  reports no recent contexts.
- **Made for you** and **Your playlists**.
- Search: type in the box at the top and press Enter. Results appear in a
  temporary **Search results** node with Tracks / Albums / Artists / Playlists.
- Single click opens an item as a foobar2000 playlist (and makes it active).
  Double click also starts playing it. The context menu has Open / Open and
  play / Add to current playlist / Add to playback queue / Sync now, then
  Follow or Unfollow (albums: Save to Your Library / Remove from Your Library),
  Rename... and Delete... for the user's own playlists, New playlist... on the
  Your playlists node, and Copy link (the `https://open.spotify.com` URL; Liked
  Songs gives `.../collection/tracks`). Top tracks / Top artists sections
  appear when a bridge fills them; today's bridge reports them empty.
- Tracks (search results, artist top tracks) open into the browse playlist
  (see below), can be dragged onto a playlist
  (`playlist_incoming_item_filter::create_dataobject_ex`), and become
  foobar2000's current selection (`ui_selection_holder`) while the tree has
  the focus, for Properties and other selection viewers. Albums, playlists
  and artists would have to be resolved first, so they are not draggable.
- Keyboard: Tab / Shift+Tab cycle search box, Refresh and tree; the Apps key
  and Shift+F10 open the item menu under the selected item.
- Per-monitor DPI: the layout is rescaled on `WM_DPICHANGED_AFTERPARENT` (and
  rechecked on `WM_SIZE`) when the window moves to another monitor.
- The search, tree loads and every bridge call run on worker threads
  (`fb2k::splitTask`), and results return through `fb2k::inMainThread`; the UI
  never blocks. Loading, connecting, not-logged-in and error states are shown
  in the tree/status line.

Dark mode: the element uses `ui_config_manager`/`is_dark_mode()`,
`ui_element_instance_callback::query_color/query_font_ex` and the host's
`fb2k::coreDarkMode` hooks (`AddControls`) for the TreeView, edit box and
button, following the SDK's UI Element and DarkMode helpers.

Columns UI: colours come from Columns UI's global colours
(`cui::colours::helper`), fonts from its common items / labels fonts
(`cui::fonts::manager`), and changes arrive through the common callbacks.
The Columns UI SDK (v8.1.0, no WTL needed) is downloaded by
`scripts/fetch_cui_sdk.ps1` into `third_party/foobar2000_sdk/columns_ui-sdk`
(it includes `../pfc` and `../foobar2000`, so it sits next to them) and is
built as a project reference. `build.ps1` calls it after `fetch_sdk.ps1`.

## Synced playlists

Opening a playlist, album, Liked Songs or a search track from the tab creates
(or reuses) a normal foobar2000 playlist named `<prefix><name>` (default
`Spotify · `) and links it to the Spotify context URI through the persistent
playlist property **`foo_librespot.context`** (a GUID property; the value is
the URI), so renaming the playlist does not break it. Further properties store
the last applied revision, the Spotify name and the library revision.

- **Lock**: every synced playlist carries a `playlist_lock` ("Synced with
  Spotify") that refuses adding, removing, reordering and replacing items,
  with a short popup that points to Detach and to Remove from Spotify
  playlist. Renaming and deleting the playlist stay allowed. The component's
  own sync and Remove from Spotify playlist pass the lock through the RAII
  guard `flp::synced_playlist_edit` (`spotify_playlists.h`). Locks are
  installed at startup and dropped on Detach. A playlist that already has
  another component's lock is left unlocked.
- **Browse playlist**: tracks (search results, artist top tracks), artist top
  tracks and radio from the Spotify submenu all reuse one `<prefix>Browse`
  playlist (property value `foo_librespot:browse`). It is not synced or
  locked.
- **Rename**: when Spotify reports a new name, the foobar2000 playlist is
  renamed to `<prefix><new name>`, but only if it still has the name the
  component gave it (a user rename is kept).

- Sync resolves the context again. If the bridge's `context.revision` matches
  the stored one, nothing is written. Otherwise a minimal diff is applied:
  removed tracks are deleted, new tracks inserted at their final positions,
  and survivors reordered to Spotify's order (`src/sync_plan.cpp`, unit-tested).
- New items get `metadb_hint_list_v3::add_hint_forced()` with their
  `file_info`, so titles, album grouping, durations and cover art grouping are
  available immediately.
- The playing item is protected when duplicate URIs make a choice possible;
  reordering never interrupts playback, and selection/focus stay attached to
  their items.
- Triggers: opening from the tab, the timer (Preferences, default 30 minutes,
  "Off" = 0), **Library > Sync Spotify playlists**, and **Sync now** in the tab.
  The timer first makes one `library` call and skips resolving playlists whose
  library revision is unchanged since the last sync. Made-for-you playlists
  report no revision and are always resolved. Manual syncs always resolve.
- Detach: **Edit > Detach from Spotify** for the active playlist (greyed out
  when it is not synced), or right-click a playlist (tab/Playlist Manager) and
  choose **Detach from Spotify** (hidden for playlists that are not synced).
  From the Playlist Manager it acts on the right-clicked playlist. The
  playlist is kept, and the link and the lock are removed.
- Removing tracks: select them in the foobar2000 copy of a synced Spotify
  playlist and choose **Remove from Spotify playlist** (Spotify submenu). The
  component sends each track's URI and its position in the foobar2000
  playlist; the bridge verifies them against Spotify's current revision,
  removes them there, and the returned revision is stored so the next sync
  does not put them back. Albums, Liked Songs, radio and detached playlists
  are not offered.
- Playlists without the context property are never touched.

## Now-playing scripts: Spotify fields

Every Spotify track carries:

- Meta `SOURCE` = `Spotify`: test with `%source%`, for example
  `$if($strcmp(%source%,Spotify),Spotify,Local file)`.
- Technical info `spotify_uri`: test with `$info(spotify_uri)`, for example
  `$if($info(spotify_uri),Spotify,Local file)`.

- Technical info `spotify_album_uri` and `spotify_artist_uri` (several artists
  joined with `, `).
- `%spotify_liked%` (a `metadb_display_field_provider`, `src/liked_cache.cpp`):
  `1` when the track is in Liked Songs. The set comes from the bridge's `liked`
  op, is re-read every 10 minutes and after a library read or login, and is
  updated at once by Save/Remove from Liked Songs; changed tracks are redrawn
  through `metadb_io::dispatch_refresh`.

They are set by `src/metadata.cpp` for preloaded playlist info and by the input
when a track is first played. The bitrate comes from the preferred-bitrate
setting in `info` requests and is corrected once from the `open` header through
`decode_get_dynamic_info()`.

## Requirements

- Visual Studio 2022/2026 with the C++ desktop workload (MSVC v143 or v145)
- PowerShell 5.1+
- 7-Zip (optional; `tar.exe` is used as fallback) and `curl.exe` for SDK fetch

The official foobar2000 SDK is downloaded on demand by
`scripts/fetch_sdk.ps1` (currently SDK 2026-10-01) into
`third_party/foobar2000_sdk/`; it is not committed.

## Build

```powershell
cd component
.\scripts\build.ps1                     # Release x64
.\scripts\build.ps1 -WithTools          # + tools/fake_bridge, tools/bridge_probe
```

`build.ps1` locates Visual Studio with `vswhere`, picks the installed platform
toolset (v145 for MSVC 14.5x) and passes it to MSBuild as
`/p:PlatformToolset=...`. The SDK's project files are not edited. It runs
`fetch_sdk.ps1` and `fetch_cui_sdk.ps1` first (both skip when present).

Products:

- `build/<Config>/x64/foo_librespot.dll`
- `build/tools/<Config>/<Platform>/fake_bridge.exe`, `bridge_probe.exe`,
  `metadata_probe.exe`, `sync_plan_test.exe`

## Package

```powershell
.\scripts\package.ps1
.\scripts\package.ps1 -BridgeExe ..\bridge\bin\librespot-bridge.exe
```

Builds Release Win32 and x64 and produces `dist/foo_librespot.fb2k-component` (a zip).
The same 64-bit bridge serves both DLLs, since it runs as its own process.

```
foo_librespot.dll          x86 DLL, for 32-bit foobar2000
librespot-bridge.exe       its helper
x64/foo_librespot.dll      x64 DLL, for 64-bit foobar2000
x64/librespot-bridge.exe   its helper
```

If the bridge executable is missing, the DLL is still packaged and a warning
is printed. The bridge is looked up next to the DLL at runtime
(`core_api::get_my_full_path()`), then in the parent folder.

## Test

```powershell
.\scripts\run_tests.ps1
.\scripts\run_tests.ps1 -Foobar2000Dir "C:\path\to\foobar2000"   # for metadata_probe's shared.dll
```

Builds everything and runs the bridge client against fake bridges
(`tools/fake_bridge.exe`, plus `tools/fake_bridge.py` when pywin32 is
available), and the metadata mapping against the real SDK `file_info` without a
host. Covered:

- status / info / resolve JSON handling, plus the v2 `library`, `search`,
  `artist` and `context.revision` replies (revision changes are driven through
  the fake bridge's test-only `test_control` op)
- `info` with the preferred bitrate returns that bitrate
- PCM streaming: 2 s @ 44100 Hz = 88200 frames, 440 Hz sine peak 0.25 / RMS
  0.176777, intact stereo interleaving
- seek: `start_ms=1000` -> `start_frame=44100`, sample-accurate remainder
- `abort_callback` interruption of a real-time stream (<2 s latency) and clean
  server-side disconnect handling
- `not_logged_in` error mapping
- `file_info` mapping: length, multi-value ARTIST, ALBUM, ALBUM ARTIST,
  TRACKNUMBER, DISCNUMBER, DATE, `SOURCE=Spotify`, `spotify_uri`, codec,
  samplerate, channels, bitrate, `REPLAYGAIN_TRACK_GAIN`/`PEAK` and album gains
- the sync diff as a pure function (`tools/sync_plan_test`): inserts, removals,
  reorders, duplicates, protected playing item, complete replacement
- v5 `following` / `follow` / `unfollow`, `playlist_create` / `rename` /
  `delete` / `remove` (including stale-index and wrong-URI refusals) and the
  new `recently_played` / `top_tracks` / `top_artists` sections, against both
  fake bridges
- bridge process spawning (`--parent-pid`, `--pipe`, `--debug`) and pipe wait
- v6: `status` pid/parent_pid/debug, `liked`, `logout`, `report_play`
  validation, `config` debug, `shutdown` refused for a non-owner; `stop_owned`
  shuts down a spawned bridge, leaves a foreign one (another foobar2000) and a
  leftover of an exited foobar2000 running, and reports a version mismatch
- `bridge_probe rules`: truncation tolerance, when a play counts, Spotify's
  reason words and the console rate limiter (`src/playback_rules.cpp`)

Manual client use:

```powershell
python tools\fake_bridge.py --pipe foo_librespot --duration-ms 5000
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot status
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot library
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot search "nation of language"
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot following spotify:artist:ID spotify:album:ID
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot playlist_create "New Mix"
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot playlist_remove spotify:playlist:ID spotify:track:ID:3
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot play spotify:track:0123456789abcdefghijkl --frames all
```

The fake bridge's test-only `control` op changes its revision/track count so a
resync can be exercised without Spotify:

```powershell
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot control --revision 2 --tracks 5
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot resolve spotify:playlist:user000000000000000001
```

## How `spotify:` paths are accepted

The component registers a normal `input_entry` (`input_singletrack_factory_t`)
whose `g_is_our_path()` accepts `spotify:track:`; the input does **not** call
`input_open_file_helper()` and ignores the optional `file` hint, because no
filesystem object is involved. foobar2000's input manager resolves the entry by
path (the same mechanism foo_input_spotify used), so playlist items whose path is
`spotify:track:<id>` load metadata and decode without a filesystem service.

`File > Add Spotify link...` does not rely on foobar2000's URL/playlist import
path at all: it resolves the links through the bridge, creates `metadb_handle`s
for the resulting `spotify:track:` paths, preloads their metadata via
`metadb_hint_list_v3::add_hint_forced()`, and inserts them into the active
playlist. This keeps the "protocol is not a filesystem" question out of the
import path.

## Error behaviour

- Bridge not running: it is started hidden with
  `--parent-pid <foobar2000 pid> --pipe foo_librespot` on first use. Readiness
  is the pipe accepting connections (`WaitNamedPipe`), never a status round
  trip, and the start timeout is 15 s. A bridge this process already started is
  never duplicated: if its handle is still alive, the component waits for its
  pipe instead of spawning another.
- Spotify still connecting: `info`/`resolve` wait up to 60 s, an `open` header
  up to 30 s, all abortable by foobar2000's abort callback. A failed session
  comes back as `unavailable` with `connecting to Spotify failed: <reason>`,
  which the component shows as `Spotify: connecting failed — <reason>`.
- Bridge replies `not_logged_in`: the input throws
  `Spotify: not logged in — Preferences > Tools > Spotify` and details go
  to the foobar2000 console.
- Specific failures (protocol v4, `src/error_policy.cpp`, unit-tested in
  `tools/sync_plan_test`): `premium_required`, `rate_limited` (with the
  retry-after seconds when known), `region_unavailable`, `track_unavailable`.
  Playing a track that fails prints one console line per track
  (`foo_librespot: <spotify:track:...>: <message> (code: ...)`) and raises a
  short popup **once per kind and session**, so a playlist of unavailable
  tracks does not raise a dialog per track. `track_unavailable` and
  `region_unavailable` (and a track `info` reports as not playable) are thrown
  as `exception_io_not_found`, the per-item failure foobar2000 handles by moving
  on to the next playlist item; the other kinds (login, Premium, rate limit,
  connection) throw `exception_io_denied` because they are not about one track.
  Old bridges' generic `unavailable` / `internal` replies keep showing the
  bridge's message as before.

## Audio cache

Preferences > Tools > Spotify: **Audio cache size** (Off, 256 MB, 512 MB,
1 GB (default), 2 GB, 5 GB, 10 GB) and **Clear cache**, with the amount cached
shown next to it. The files are the encrypted audio from Spotify's CDN in
`%LOCALAPPDATA%\foo_librespot\cache`. The setting is passed to a starting
bridge as `--cache-mb N`; when Apply is pressed while the bridge is already
running it is applied live through the `config` op (no restart, playback is
not interrupted). Clear cache goes through the running bridge, or deletes the
folder directly when none runs. A track is cached once it has been played to the
end of its download; skipping half way caches nothing.

## ReplayGain

Track and album gain/peak come from Spotify's loudness data (`-18 - loudness`,
see `proto/PROTOCOL.md`). If `info` could not tell the album values, or the
playing file's values differ (relinking), the `open` header's values are handed
to foobar2000 as dynamic info once playback starts, so album gain mode follows.

## Layout

```
src/main.cpp               component identity
src/input_spotify.cpp      input_entry + decoder (float32 PCM, seek by reopen)
src/add_link.cpp           File > Add Spotify link... + dialog (threaded_process)
src/preferences.cpp        Tools > Spotify preferences page
src/spotify_tab.*          Spotify panel (search + library tree) + Default UI element
src/spotify_tab_cui.cpp    Columns UI panel host (uie::window)
src/spotify_playlists.*    context property, create/reuse, minimal-diff sync, lock
src/app_cancel_shim.*      temporary app-wide cancel token (until merged)
src/sync_plan.*            pure diff algorithm (unit-tested)
src/spotify_commands.cpp   sync-all menu, detach menu/context item, sync timer
src/album_art_spotify.cpp  cover_front extractor (http_client download)
src/component_helpers.*    path setup, abort bridging, file_info population
src/bridge/json.*          minimal JSON parser/writer (SDK-free)
src/bridge/pipe.*          interruptible overlapped named-pipe stream
src/bridge/protocol.*      PROTOCOL.md v1 + v2 client
src/bridge/process.*       librespot-bridge.exe lifetime management
src/foo_librespot.rc       dialogs + version resource
tools/                     fake bridges, console harnesses, metadata/sync tests
scripts/                   fetch_sdk / fetch_cui_sdk / build / test / package
```

## Notes / limitations

- The component and tools use only the SDK, pfc and the component client (plain
  Win32 dialogs and controls); ATL/WTL from `foo_sample` is not required, which
  keeps the build self-contained (the SDK does not ship WTL).
- The bridge executable is built from `../bridge` (`bridge\build.ps1`) and
  supplied to packaging with `-BridgeExe` (the default points at it).
- Pasted and dropped Spotify links go through a `filesystem` service
  (`src/spotify_filesystem.cpp`), not the add-link dialog.
- `retag` is not supported (virtual tracks).
- Decoding without `input_flag_playback` (converter, ReplayGain scanner, other
  full decodes) throws `exception_io_denied`; `get_info` keeps working. A
  stream that ends cleanly more than 2 s (or 1 %) before the track's duration
  throws `exception_io_data` instead of counting as finished.
- On quit (`initquit::on_quit`) the component sends `shutdown` to the bridge it
  spawned and waits up to 3 s, then terminates that process only. A bridge
  another foobar2000 instance started is never stopped.
- Plays are reported through `play_callback_static` (`src/play_reporter.cpp`):
  30 s or more, or a short track played to its end. Each report is sent once
  from a worker thread and never retried; the console shows the outcome. Off
  with "Report plays to Spotify" in the preferences. Not verified against a
  live account (only the bridge's dry run).
- Album art: images are cached by URL across tracks (96 images / 48 MB),
  artist pictures come from the `artist` op, and failures print at most one
  console line per kind and minute.
- The Spotify tab, its dark-mode rendering and the playlist sync operate on
  foobar2000 APIs that cannot run outside the host; they are built for
  x64 and the protocol layer/diff are covered by the tests above, but the
  in-app UI still needs a real foobar2000 run to be verified visually.
- Columns UI dark mode: the panel follows Columns UI's dark mode flag for
  its colours. Scroll bars and the edit box use foobar2000's core dark mode
  hooks, so they can differ if the two settings disagree.
