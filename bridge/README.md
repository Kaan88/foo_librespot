# foo_librespot bridge — Phase 0 spike

Proof of concept for the future foobar2000 plugin: use
[`github.com/devgianlu/go-librespot`](https://github.com/devgianlu/go-librespot)
as a **library** on Windows to log in, fetch one Spotify track, decode it to
float32 PCM, and write 16-bit WAV — without the Spotify Connect state machine and
without opening an audio output device.

The module is self-contained (`foo_librespot/bridge`) so the plugin can later
import the same code.

## Layout

```
bridge/
  cmd/spike/main.go       CLI: login, dump, flag parsing
  internal/spike/         session/player factory, device-auth login, dump loop,
                          track-id parsing, loudness reporting
  internal/wav/           streaming 16-bit WAV writer (+ unit tests)
  internal/creds/         %APPDATA%\foo_librespot\credentials.json
  internal/logging/       go-librespot Logger adapter
  build.ps1               build spike.exe with cgo + MinGW libraries
```

## Build

```powershell
cd bridge
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

`build.ps1` prepends `C:\Program Files\Go\bin` and `C:\msys64\mingw64\bin` to
`PATH`, sets `CGO_ENABLED=1`, `CC=gcc`, `PKG_CONFIG_PATH`, and builds
`bin\spike.exe`. The C decode libraries (libogg, libvorbis, libFLAC, mpg123) are
**linked statically**, so the resulting exe only imports system DLLs
(KERNEL32, msvcrt, SHLWAPI) and needs no MinGW DLLs at runtime.

Manual equivalent:

```powershell
$env:PATH = "C:\Program Files\Go\bin;C:\msys64\mingw64\bin;$env:PATH"
$env:CGO_ENABLED = "1"
$env:CC = "gcc"
$env:PKG_CONFIG_PATH = "C:\msys64\mingw64\lib\pkgconfig"
$env:CGO_CFLAGS = "-DFLAC__NO_DLL"
$env:CGO_LDFLAGS = "-lshlwapi"
go build -trimpath -ldflags "-linkmode external -extldflags -static" -o bin/spike.exe ./cmd/spike
```

Why those two cgo flags:

- `-DFLAC__NO_DLL` — FLAC's headers declare `dllimport` on Windows unless this is
  defined; linking `libFLAC.a` otherwise fails on `__imp_FLAC__*`.
- `-lshlwapi` — mpg123's static archives call `PathIsRelativeW` and friends.

Static linking is also required for libogg: MSYS2's `libogg-0.dll` does not
export `ogg_stream_iovecin`, which `xlab/vorbis-go` references, so a default
(dynamic) link fails; `libogg.a` has the symbol.

Checks:

```powershell
go vet ./...
go test -ldflags "-linkmode external -extldflags -static" ./...
.\bin\spike.exe --help
```

## Usage

```text
spike.exe login
spike.exe dump <spotify:track:ID | open.spotify.com/track/ID | ID> <out.wav>
              [--start-ms N] [--seconds S] [--bitrate 96|160|320] [-v]
```

### login

Runs go-librespot's OAuth 2.0 **device authorization** flow. It prints the
verification URL and user code, waits for approval, then stores:

```json
{
  "version": 1,
  "device_id": "<40 hex chars>",
  "device_type": 1,
  "username": "<spotify user>",
  "data": "<base64 stored-credentials blob>",
  "saved_at": "..."
}
```

at `%APPDATA%\foo_librespot\credentials.json`. Later commands reuse the same
device id and stored credentials.

### dump

```powershell
# whole track at the preferred 160 kbps
.\bin\spike.exe dump spotify:track:4uLU6hMCjMI75M1A2tKUQC out.wav

# 15 s starting at 1:00, ask for 320 kbps, from a web URL
.\bin\spike.exe dump https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC out.wav `
    --start-ms 60000 --seconds 15 --bitrate 320
```

The WAV is always 44100 Hz, stereo, 16-bit PCM. The command prints track name,
artists, duration, selected file format/codec/bitrate, decoder sample rate and
bit depth, Spotify loudness/true-peak metadata, decoded level (peak/RMS), and
seek positions reported by the decoder.

## How it works

### Session and login

Built directly on `session.NewSessionFromOptions`:

- `login` → `session.DeviceAuthCredentials` with an `OnCode` callback that prints
  the URL/code.
- `dump` → `session.StoredCredentials{Username, Data}`.

Device type is `DeviceType_COMPUTER`; the 20-byte device id is generated once at
login and persisted. The session object supplies:

- `Spclient()` — metadata, storage resolution, audio files extension;
- `AudioKey()` — audio-key provider;
- `Events()` — go-librespot's default event manager (`events.Plugin`), which is a
  no-op unless an events plugin is installed.

No dealer/Connect state, zeroconf, or state reporting is driven by this tool.

### Player and stream

`player.NewPlayer` is called with only `Spclient`, `AudioKey`, `Events`, `Log`,
`CountryCode`, and `NormalisationEnabled: false`. `NewPlayer` merely stores an
output-factory closure; the audio backend is opened only by
`SetPrimaryStream`, which is **never called**. Consequently no WASAPI/ALSA/Pulse
device is opened.

`player.NewStream(ctx, httpClient, spotifyId, bitrate, mediaPosition)` then:

1. resolves track metadata (`getUnrestrictedTrack`) and the audio-files
   extension;
2. selects the available format nearest the requested bitrate
   (`selectBestMediaFormat`; FLAC only when enabled, which this spike doesn't);
3. requests the audio key, resolves storage, downloads the encrypted file;
4. decrypts it and builds a Vorbis/MP3/FLAC decoder that implements
   `librespot.AudioSource`;
5. seeks to `mediaPosition` using `player.StartPosition` (clamps to 0 at/after
   track end) and `Source.SetPositionMs`.

`Stream.Source` is the decoder: `Read([]float32) (int, error)` returns
**interleaved float32 samples** (already 44100 Hz stereo for all supported
formats; the library rejects anything else). The dump loop reads 16384 floats at
a time, tracks peak/RMS, and the WAV writer converts to signed 16-bit with
clipping.

### Seeking

`--start-ms` is passed as `NewStream`'s `mediaPosition`, so the seek is performed
inside the same call that opens the stream (`SetPositionMs` on the decoder, which
uses each codec's seek table/byte offset). The tool prints the position reported
by `Source.PositionMs()` immediately after opening and after decoding, and warns
if the open position is more than 3 s from the requested one. Requesting a
position at or beyond the duration starts at 0 by design.

### Normalisation metadata

Loudness is fetched separately from the audio-files extension
(`DefaultFileNormalizationParams`: loudness LUFS, true peak dBTP). The spike
decodes **without** applying normalisation (`NormalisationEnabled: false`) so the
plugin can decide; it prints the gain/factor go-librespot would apply.

## Open questions / unverified assumptions

- **Not exercised against live Spotify here.** Logging in and downloading need the
  user's interactive approval; build, vet, tests, and `--help` were verified.
- Stored-credential expiry/refresh over long periods is not tested. The blob is
  persisted once; if Spotify invalidates it, re-run `login`.
- The empty `CountryCode` passed to the player disables go-librespot's
  restricted-track check (the library skips the check for non-2-letter codes).
  A real plugin should pass the user's country; with a country set, the
  library's alternative-track relinking applies.
- `Read` EOF behaviour differs slightly between decoders (some return the final
  samples together with `io.EOF`); the loop writes `n` samples before treating
  EOF as completion, so both shapes work. This was read from the library source,
  not observed on a real stream.
- Decoder `Close` methods have inconsistent signatures (`Close()` vs
  `Close() error`); the tool calls whichever is present and ignores the error.
- Fetching loudness duplicates one metadata request that `NewStream` already
  makes internally; a future integration should expose it from the stream path
  instead.
- `go.mod` declares Go 1.25 (go-librespot v0.10.7's minimum); built with Go
  1.27.4 on this machine.
