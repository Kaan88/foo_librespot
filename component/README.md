# foo_librespot (foobar2000 component)

foobar2000 v2 component that plays Spotify tracks by talking to the
`librespot-bridge.exe` helper over the named-pipe protocol in
[`../proto/PROTOCOL.md`](../proto/PROTOCOL.md).

- Input: `spotify:track:<22-char id>` (44.1 kHz stereo float32 PCM from the bridge)
- **Spotify** Default UI element: a search box and a library tree (Liked Songs,
  saved albums, followed artists, made-for-you and your playlists); click an
  entry to open it as a foobar2000 playlist, double-click to also start playing
- Synced playlists named `Spotify · <name>`, tracked by the Spotify context URI
  and updated with a minimal diff on open, on a timer and on demand
- File > **Add Spotify link...**: resolves track / album / playlist URIs and
  `open.spotify.com` URLs into playlist entries
- Album art: front cover downloaded from the bridge's `cover_url` via
  foobar2000's `http_client`
- Preferences > Tools > **foo_librespot**: bridge status, Log in..., bitrate,
  sync interval and playlist name prefix

## Spotify tab

**Add New UI Element...** lists it as **Spotify** (Media Library viewers), so
it can be dropped into a Default UI tab stack next to Playlists / Properties /
ReFacets. It is a plain Win32 TreeView (comctl32); no ATL/WTL is required.

- **Your library**: Liked Songs, Albums (saved), Artists (followed; expand an
  artist to load its albums and singles through the bridge `artist` op).
- **Made for you** and **Your playlists**.
- Search: type in the box at the top and press Enter. Results appear in a
  temporary **Search results** node with Tracks / Albums / Artists / Playlists.
- Single click opens an item as a foobar2000 playlist (and makes it active).
  Double click also starts playing it. The context menu has Open / Open and
  play / Add to current playlist / Sync now / Copy link.
- The search, tree loads and every bridge call run on worker threads
  (`fb2k::splitTask`), and results return through `fb2k::inMainThread`; the UI
  never blocks. Loading, connecting, not-logged-in and error states are shown
  in the tree/status line.

Dark mode: the element uses `ui_config_manager`/`is_dark_mode()`,
`ui_element_instance_callback::query_color/query_font_ex` and the host's
`fb2k::coreDarkMode` hooks (`AddControls`) for the TreeView, edit box and
button, following the SDK's UI Element and DarkMode helpers.

Columns UI is **not** built: its SDK is not vendored in this repository, and
adding the panel would require downloading a second SDK and its WTL/ATL
dependencies. The Default UI element covers the tabbed layout described in the
roadmap.

## Synced playlists

Opening a playlist, album, Liked Songs or a search track from the tab creates
(or reuses) a normal foobar2000 playlist named `<prefix><name>` (default
`Spotify · `) and links it to the Spotify context URI through the persistent
playlist property **`foo_librespot.context`** (a GUID property; the value is
the URI), so renaming the playlist does not break it. A second property stores
the last applied revision.

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
- Detach: **Edit > Detach from Spotify** for the active playlist, or right-click
  a playlist (tab/Playlist Manager) and choose **Detach from Spotify**. The
  playlist is kept, only the link is removed.
- Playlists without the context property are never touched.

## Now-playing scripts: Spotify fields

Every Spotify track carries:

- Meta `SOURCE` = `Spotify`: test with `%source%`, for example
  `$if($strcmp(%source%,Spotify),Spotify,Local file)`.
- Technical info `spotify_uri`: test with `$info(spotify_uri)`, for example
  `$if($info(spotify_uri),Spotify,Local file)`.

Both are set by `src/metadata.cpp` for preloaded playlist info and by the input
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
.\scripts\build.ps1 -Platform Win32     # the 32-bit build foobar2000 x86 loads
.\scripts\build.ps1 -WithTools          # + tools/fake_bridge, tools/bridge_probe
```

`build.ps1` locates Visual Studio with `vswhere`, picks the installed platform
toolset (v145 for MSVC 14.5x) and passes it to MSBuild as
`/p:PlatformToolset=...`. The SDK's project files are not edited.

Products:

- `build/<Config>/x64/foo_librespot.dll`
- `build/<Config>/Win32/foo_librespot.dll`
- `build/tools/<Config>/<Platform>/fake_bridge.exe`, `bridge_probe.exe`,
  `metadata_probe.exe`, `sync_plan_test.exe`

## Package

```powershell
.\scripts\package.ps1
.\scripts\package.ps1 -BridgeExe ..\bridge\bin\librespot-bridge.exe
```

Builds both platforms and produces `dist/foo_librespot.fb2k-component` (a zip)
in the foobar2000 v2 multi-architecture layout:

```
foo_librespot.dll          x86 DLL, at the component root
librespot-bridge.exe       its helper
x64/foo_librespot.dll      x64 DLL
x64/librespot-bridge.exe   its helper
```

If the bridge executable is missing, the DLLs are still packaged and a warning
is printed. The bridge is looked up next to the DLL at runtime
(`core_api::get_my_full_path()`), then in the parent folder, so a DLL copied out
of its architecture folder still finds a bridge at the package root.

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
- bridge process spawning (`--parent-pid`, `--pipe`) and pipe wait

Manual client use:

```powershell
python tools\fake_bridge.py --pipe foo_librespot --duration-ms 5000
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot status
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot library
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot search "nation of language"
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
  `Spotify: not logged in — Preferences > Tools > foo_librespot` and details go
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

Preferences > Tools > foo_librespot: **Audio cache size** (Off, 256 MB, 512 MB,
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
src/add_link.cpp           File > Add Spotify link... + dialog
src/preferences.cpp        Tools > foo_librespot preferences page
src/spotify_tab.*          Spotify Default UI element (search + library tree)
src/spotify_playlists.*    context property, create/reuse, minimal-diff sync
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
scripts/                   fetch_sdk / build / test / package
```

## Notes / limitations

- The component and tools use only the SDK, pfc and the component client (plain
  Win32 dialogs and controls); ATL/WTL from `foo_sample` is not required, which
  keeps the build self-contained (the SDK does not ship WTL).
- The bridge executable is built from `../bridge` (`bridge\build.ps1`) and
  supplied to packaging with `-BridgeExe` (the default points at it).
- Clipboard paste / drag-and-drop of Spotify URLs is not wired to the add-link
  resolver (nice-to-have, not implemented).
- `retag` is not supported (virtual tracks).
- The Spotify tab, its dark-mode rendering and the playlist sync operate on
  foobar2000 APIs that cannot run outside the host; they are built for x86 and
  x64 and the protocol layer/diff are covered by the tests above, but the
  in-app UI still needs a real foobar2000 run to be verified visually.
- Columns UI is not supported (see "Spotify tab").
