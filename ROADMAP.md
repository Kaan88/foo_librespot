# foo_librespot roadmap

A foobar2000 component that plays Spotify through foobar2000's own pipeline (output, DSP, ReplayGain). It's built on go-librespot, which runs in a separate bridge process.

## Done

### Phase 0: proof of concept ✅
- go-librespot used as a library on Windows: log in, decode one track to float audio, write a WAV.

### Phase 1: bridge ✅
- `librespot-bridge.exe` talks to the component over a named pipe (`proto/PROTOCOL.md`): status, resolve, info, open.
- Seeks start on the exact sample. Uses the account's country, so tracks swapped for your region play.
- `status` answers instantly; the Spotify login runs in the background, once, and retries on failure.
- Handles Spotify-generated playlists (Daily Mix and similar), albums, playlists and Liked Songs.

### Phase 2: playback in foobar2000 ✅
- `spotify:track:` entries play with seeking. Tags, ReplayGain values (from Spotify's loudness data) and album art all work.
- File > Add Spotify link...; a preferences page (login, bitrate 96/160/320).
- The 32-bit build runs in the user's portable foobar2000. The package contains x86 and x64 builds.

## Next

### Phase 3: Spotify tab and synced playlists ✅

**Spotify tab** (a panel of its own, next to Playlists / Properties / ReFacets)
- A tree:
  - Your library: Liked Songs, Albums, Artists.
  - Made for you: Daily Mixes, Discover Weekly, Release Radar.
  - Your playlists.
- Search box at the top.
- Clicking an entry opens it as a normal foobar2000 playlist. All playback goes through the default playlist views, so the user's columns, album grouping and art keep working.
- Built as both a Default UI element and a Columns UI panel, so it fits tabbed layouts.

**Synced playlists**
- A Spotify playlist opened from the tab becomes a real foobar2000 playlist named "Spotify · <name>". Spotify is the source of truth for it.
- Sync adds new tracks, removes deleted ones and keeps the order. It runs on open, on a timer (configurable) and from a manual "Sync now".
- Track info is cached locally so large playlists load instantly.
- Never touches non-Spotify playlists. Playlists the user has edited locally can be detached from sync.

**Bridge additions**
- Library listing (saved albums, followed artists, the user's playlists, "made for you").
- Search.
- Artist and album pages.

### Phase 4: right-click menu ✅ (works in foobar2000, 2026-11-23)
- A Spotify submenu: Save to Liked Songs, Open album, Open artist, Start radio, Add to Spotify playlist, Copy link.
- Write actions (save, edit playlists, radio) need a Spotify Web API token. Check first that one can come from the existing librespot login.
- Drag and drop or paste `open.spotify.com` / `spotify:` links straight into playlists. This needs a filesystem service for `spotify:` paths.

### Phase 5: polish (optional): cache, errors, album gain done; Spotify Connect left ⬅ next
- ✅ Audio cache size setting (preferences: Off..10 GB, default 1 GB, Clear cache; `--cache-mb` at spawn, `config` op live). Verified in foobar2000 x64 (2026-11-23).
- ✅ Clearer errors: `premium_required`, `rate_limited`, `region_unavailable`, `track_unavailable`; popup once per kind; unavailable tracks are skipped. Verified in foobar2000 x64 (2026-11-23).
- ✅ Album ReplayGain values (AUDIO_FILES `default_album_normalization_params`, also from the open header as dynamic info).
- Spotify Connect: control foobar2000 from the phone (largest item, last).

## Known issues / fix notes

- **Now-playing "BITRATE ? kbps".** The bitrate is only known once a stream has been opened, so `info` doesn't return it and `get_info` leaves it out. Fix: return the selected format's bitrate (from the preferred bitrate setting) in `info`, and update it from the `open` header during playback (dynamic info).
- **Now-playing "SOURCE Local file".** The user's now-playing panel script decides the source from the path. Fix options:
  - (a) the component adds a tag such as `SOURCE=Spotify` (or `%__spotify%`) that the script can read;
  - (b) change the script to treat paths starting with `spotify:` as Spotify.
  - Do (a) and tell the user which field to use.
- **Seek accuracy relies on a private go-librespot value** (the Vorbis decoder's granule position, read through reflection). A go-librespot upgrade could break it; the bridge then decodes from the start of the track, which is exact but slower. Recheck after every go-librespot update.
- **Known Phase 4 limits:** search can't return playlists (no endpoint reachable from this login); `playlist_add` hasn't had a real write yet (dry run only); dropping `https://open.spotify.com` URLs depends on foobar2000 asking our filesystem before its HTTP one.
- **Leftover worktree folders** `Desktop\fbp-bridge`, `Desktop\fbp-component` and `Desktop\foobar_plugin-phase0` are locked and still need deleting, along with the `bridge-v1` / `component-v1` branches.

## Working agreement
- The implementing model (DeepSeek Flash earlier, now cc-sonnet = opencode `claude-code/sonnet`) writes the code on its own branch and worktree. Claude reviews it, rebuilds independently and merges into main.
- Install target: portable 32-bit foobar2000 at `Desktop\foobar2000`, in `profile\user-components\foo_librespot\`. Close foobar2000 before swapping the DLL.
