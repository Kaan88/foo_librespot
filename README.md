# foo_librespot: Spotify for foobar2000

**foo_librespot** is a [foobar2000](https://www.foobar2000.org/) v2 component that plays Spotify inside foobar2000 itself. Tracks go through foobar2000's own output, DSP chain and ReplayGain, they sit in normal playlists with your columns and album art, and your Spotify library is a panel next to everything else.

It is built on [go-librespot](https://github.com/devgianlu/go-librespot), an open-source Spotify client library, which runs in a small helper process (`librespot-bridge.exe`) that the component starts and stops on its own.

> Requires a **Spotify Premium** account, **Windows** and **64-bit foobar2000 v2**.

## Features

- **Plays `spotify:track:` entries like local files**: sample-accurate seeking, 96 / 160 / 320 kbps, and your output device, DSPs and ReplayGain all apply.
- **Spotify panel** (a Default UI element, and a Columns UI panel): search, Liked Songs, saved albums, followed artists (with their albums and singles), Made for you (Daily Mixes, Discover Weekly, Release Radar), your playlists and Recently played. Click an entry to open it as a foobar2000 playlist; double-click to play it. Right-click an artist, album or playlist to follow it or save it (or remove it from your library), create, rename and delete playlists, and add an album, playlist or track to the playback queue. Drag tracks from the panel onto a playlist. The keyboard works too: Tab moves between the search box and the tree, and the Apps key or Shift+F10 opens the menu. **Copy link** copies an `https://open.spotify.com` link.
- **Browse playlist**: tracks from search, artist pages and the Spotify submenu (artist top tracks, radio) open in one reusable `Spotify · Browse` playlist instead of a new playlist each time.
- **Synced playlists**: a Spotify playlist, album or Liked Songs becomes a foobar2000 playlist called `Spotify · <name>`. It stays in step with Spotify (new tracks added, removed ones dropped, order kept) when you open it, on a timer and through **Library > Sync Spotify playlists**. To take tracks out of the Spotify playlist itself, select them and use **Spotify > Remove from Spotify playlist**. If a playlist is renamed on Spotify, the foobar2000 playlist follows (unless you renamed it yourself). Synced playlists are locked against edits in foobar2000, since the next sync would undo them. You can detach a playlist to unlock it and keep it as it is.
- **Right-click Spotify submenu**: Save to / Remove from Liked Songs, Open album, Open artist, Start radio, Add to Spotify playlist, Remove from Spotify playlist, Copy link.
- **Add links**: **File > Add Spotify link...** takes track, album and playlist links (`spotify:...` or `https://open.spotify.com/...`) and resolves them in the background with a progress window you can cancel. You can also paste links or drop them onto a playlist.
- **Tags, album art and ReplayGain**: title, artists, album, album artist, track/disc number and year; front covers; track and album gain from Spotify's loudness data.
- **Audio cache**: tracks you've played are kept locally (default 1 GB, adjustable from Off to 10 GB), so replays don't need the network.
- **Clear errors**: messages for "Premium required", "rate limited", "not available in your region" and "track unavailable". Unavailable tracks are skipped instead of stopping playback.
- Supports dark mode.

## Install

1. Get `foo_librespot.fb2k-component` from the [Releases](../../releases) page, or [build it yourself](#building).
2. In foobar2000: **File > Preferences > Components > Install...**, pick the file, apply and restart.
3. **File > Preferences > Tools > foo_librespot > Log in...**. A login window opens; finish the Spotify login there. Your credentials are saved to `%APPDATA%\foo_librespot\credentials.json` on your PC and go nowhere else.
4. Add the panel: in layout editing mode, right-click a panel area and choose **Add New UI Element... > Spotify**. A tab stack next to Playlists / Properties works well.

The same preferences page sets the bitrate, the playlist sync interval, the synced playlist name prefix and the audio cache size.

### Telling Spotify tracks apart in title formatting

Every Spotify track has `%source%` = `Spotify` and the technical field `$info(spotify_uri)`, for example:

```
$if($strcmp(%source%,Spotify),Spotify,Local file)
```

## How it works

```
foobar2000 ── foo_librespot.dll ──(named pipe, JSON + float PCM)── librespot-bridge.exe ── Spotify
             C++ component                                         Go, go-librespot
```

- **[`component/`](component/)**: the foobar2000 component in C++ with the official SDK: input, Spotify panel, playlist sync, menus, preferences.
- **[`bridge/`](bridge/)**: the helper process in Go. It holds one Spotify session, decodes tracks to 44.1 kHz stereo float PCM and serves them over `\\.\pipe\foo_librespot`, restricted to your Windows user. It quits when foobar2000 does.
- **[`proto/PROTOCOL.md`](proto/PROTOCOL.md)**: the wire protocol between the two.
- **[`ROADMAP.md`](ROADMAP.md)**: what's done and what's next.

The bridge logs to `%APPDATA%\foo_librespot\bridge.log`. Look there first if something fails.

## Building

You need Windows with:

- Visual Studio 2022 or newer with the C++ desktop workload
- Go 1.25+ and [MSYS2](https://www.msys2.org/) MinGW-w64 with `gcc`, `pkg-config`, libogg, libvorbis, flac and mpg123 (the bridge links them statically)
- PowerShell 5.1+

```powershell
# 1. The bridge -> bridge\bin\librespot-bridge.exe
cd bridge
powershell -ExecutionPolicy Bypass -File .\build.ps1

# 2. The component (downloads the foobar2000 and Columns UI SDKs on first run) -> component\dist\foo_librespot.fb2k-component
cd ..\component
.\scripts\fetch_sdk.ps1
.\scripts\package.ps1
```

Tests:

```powershell
cd bridge;    go test -ldflags "-linkmode external -extldflags -static" ./...
cd component; .\scripts\run_tests.ps1     # set FOOBAR2000_DIR to also run the metadata test
```

See [`bridge/README.md`](bridge/README.md) and [`component/README.md`](component/README.md) for details.

## Limitations

- Spotify Premium only; free accounts can't stream through librespot.
- Search returns tracks, albums, artists and playlists. Top tracks and top artists aren't reachable from this kind of login, so the panel doesn't show them.
- Only tracks can be dragged out of the panel; albums and playlists have to be opened first.
- Spotify Connect (controlling foobar2000 from your phone) isn't there yet. It's the next item on the [roadmap](ROADMAP.md).
- Seek accuracy relies on an internal go-librespot value. If a future go-librespot changes it, seeking stays exact but gets slower.

## Disclaimer

This is an unofficial project. It is not affiliated with, endorsed by or connected to Spotify AB or foobar2000. "Spotify" is a trademark of Spotify AB. Use it with your own Premium account and at your own risk. Using third-party clients may go against Spotify's terms of use. The component only plays audio for you to listen to and offers no way to save or download tracks.

## License

[GPL-3.0](LICENSE), because the bridge is built on go-librespot, which is GPL-3.0.

## Credits

- [go-librespot](https://github.com/devgianlu/go-librespot) by devgianlu, and the [librespot](https://github.com/librespot-org/librespot) project it descends from
- [foobar2000](https://www.foobar2000.org/) and its SDK by Peter Pawłowski
