# foo_librespot (foobar2000 component)

foobar2000 v2 component that plays Spotify tracks by talking to the
`librespot-bridge.exe` helper over the named-pipe protocol in
[`../proto/PROTOCOL.md`](../proto/PROTOCOL.md).

- Input: `spotify:track:<22-char id>` (44.1 kHz stereo float32 PCM from the bridge)
- File > **Add Spotify link...**: resolves track / album / playlist URIs and
  `open.spotify.com` URLs into playlist entries
- Album art: front cover downloaded from the bridge's `cover_url` via
  foobar2000's `http_client`
- Preferences > Tools > **foo_librespot**: bridge status, Log in..., bitrate

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
.\scripts\build.ps1 -Platform Win32     # optional 32-bit build
.\scripts\build.ps1 -WithTools          # + tools/fake_bridge, tools/bridge_probe
```

`build.ps1` locates Visual Studio with `vswhere`, picks the installed platform
toolset (v145 for MSVC 14.5x) and passes it to MSBuild as
`/p:PlatformToolset=...`. The SDK's project files are not edited.

Products:

- `build/<Config>/x64/foo_librespot.dll`
- `build/tools/<Config>/x64/fake_bridge.exe`, `bridge_probe.exe`

## Package

```powershell
.\scripts\package.ps1
.\scripts\package.ps1 -BridgeExe ..\bridge\bin\librespot-bridge.exe
```

Produces `dist/foo_librespot.fb2k-component` (a zip) with the foobar2000 v2
x64 layout:

```
x64/foo_librespot.dll
x64/librespot-bridge.exe
```

If the bridge executable is missing, the DLL is still packaged and a warning is
printed. The bridge is looked up next to the DLL at runtime
(`core_api::get_my_full_path()`), so the two files must stay together.

## Test

```powershell
.\scripts\run_tests.ps1
.\scripts\run_tests.ps1 -Foobar2000Dir "C:\path\to\foobar2000"   # for metadata_probe's shared.dll
```

Builds everything and runs the bridge client against fake bridges
(`tools/fake_bridge.exe`, plus `tools/fake_bridge.py` when pywin32 is
available), and the metadata mapping against the real SDK `file_info` without a
host. Covered:

- status / info / resolve JSON handling
- PCM streaming: 2 s @ 44100 Hz = 88200 frames, 440 Hz sine peak 0.25 / RMS
  0.176777, intact stereo interleaving
- seek: `start_ms=1000` -> `start_frame=44100`, sample-accurate remainder
- `abort_callback` interruption of a real-time stream (<2 s latency) and clean
  server-side disconnect handling
- `not_logged_in` error mapping
- `file_info` mapping: length, multi-value ARTIST, ALBUM, ALBUM ARTIST,
  TRACKNUMBER, DISCNUMBER, DATE, codec, samplerate, channels, bitrate,
  `REPLAYGAIN_TRACK_GAIN`/`PEAK` and album gains
- bridge process spawning (`--parent-pid`, `--pipe`) and pipe wait

Manual client use:

```powershell
python tools\fake_bridge.py --pipe foo_librespot --duration-ms 5000
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot status
build\tools\Release\x64\bridge_probe.exe --pipe foo_librespot play spotify:track:0123456789abcdefghijkl --frames all
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
  `--parent-pid <foobar2000 pid> --pipe foo_librespot` on first use and waited
  for up to 12 s (thread-safe, restarted if it died).
- Bridge replies `not_logged_in`: the input throws
  `Spotify: not logged in — Preferences > Tools > foo_librespot` and details go
  to the foobar2000 console.

## Layout

```
src/main.cpp               component identity
src/input_spotify.cpp      input_entry + decoder (float32 PCM, seek by reopen)
src/add_link.cpp           File > Add Spotify link... + dialog
src/preferences.cpp        Tools > foo_librespot preferences page
src/album_art_spotify.cpp  cover_front extractor (http_client download)
src/component_helpers.*    path setup, abort bridging, file_info population
src/bridge/json.*          minimal JSON parser/writer (SDK-free)
src/bridge/pipe.*          interruptible overlapped named-pipe stream
src/bridge/protocol.*      PROTOCOL.md v1 client
src/bridge/process.*       librespot-bridge.exe lifetime management
src/foo_librespot.rc       dialogs + version resource
tools/                     fake bridges, console harness, metadata unit test
scripts/                   fetch_sdk / build / test / package
```

## Notes / limitations

- The component and tools use only the SDK, pfc and the component client (plain
  Win32 dialogs); ATL/WTL from `foo_sample` is not required, which keeps the
  build self-contained (the SDK does not ship WTL).
- The bridge executable is written in a separate branch and is not part of this
  tree; `-BridgeExe` supplies it at packaging time.
- Clipboard paste / drag-and-drop of Spotify URLs is not wired to the add-link
  resolver (nice-to-have, not implemented).
- `retag` is not supported (virtual tracks).
- In-app behaviour (foobar2000 host) was not exercised in this environment by
  design; the DLL builds as an x64 foobar2000 component
  (`foobar2000_get_interface` exported) and the protocol layer is covered by the
  tests above.
