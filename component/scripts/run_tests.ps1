# End-to-end tests for the bridge client and the bridge process manager.
#
# Builds the component and tools, then exercises proto/PROTOCOL.md against
# fake bridges:
#   - status / info / resolve via tools/fake_bridge (C++ server)
#   - open + PCM streaming: 2 s track => 88200 frames, 440 Hz sine peak/RMS
#   - seek: start_ms 1000 => start_frame 44100, sample-accurate
#   - abort_callback: interrupt a real-time stream, server survives
#   - not_logged_in error code
#   - process spawn: librespot-bridge.exe started by bridge_process
#   - the Python fake bridge (tools/fake_bridge.py) when pywin32 is available

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    # foobar2000 install whose shared.dll the metadata test borrows; skipped when unset.
    [string]$Foobar2000Dir = $env:FOOBAR2000_DIR
)

$ErrorActionPreference = "Stop"

$componentDir = Split-Path -Parent $PSScriptRoot
$probe = Join-Path $componentDir "build\tools\$Configuration\x64\bridge_probe.exe"
$fake = Join-Path $componentDir "build\tools\$Configuration\x64\fake_bridge.exe"
# The metadata probe runs against the user's foobar2000 shared.dll. The
# portable install is 32-bit, so use the Win32 probe there.
$metadataProbe = Join-Path $componentDir "build\tools\$Configuration\Win32\metadata_probe.exe"
$syncPlanTest = Join-Path $componentDir "build\tools\$Configuration\x64\sync_plan_test.exe"

& (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -WithTools
& (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -Platform Win32 -WithTools

$failures = 0
function Assert([bool]$condition, [string]$message) {
    if ($condition) {
        Write-Host "PASS  $message"
    } else {
        Write-Host "FAIL  $message" -ForegroundColor Red
        $script:failures++
    }
}

function Invoke-Probe {
    param([string[]]$Arguments)
    $output = & $probe @Arguments 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Output = $output }
}

function Wait-Pipe([string]$PipeName, [int]$TimeoutMs = 10000) {
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        & $probe --pipe $PipeName status *> $null
        if ($LASTEXITCODE -eq 0) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "pipe $PipeName did not become ready"
}

# Protocol v5: following, playlist_create/rename/delete/remove, and the new
# library sections. Runs against either fake bridge.
function Assert-ManagementOps([string]$PipeName) {
    $artist = "spotify:artist:art0000000000000000001"
    $album = "spotify:album:alb0000000000000000001"
    $playlist = "spotify:playlist:user000000000000000001"

    $r = Invoke-Probe @("--pipe", $PipeName, "library")
    Assert ($r.Output -match "section id=recently_played .*items=2") `
        "$PipeName recently played section"
    Assert ($r.Output -match "section id=top_tracks .*items=0") "$PipeName top tracks section is empty"
    Assert ($r.Output -match "section id=top_artists .*items=0") "$PipeName top artists section is empty"

    $r = Invoke-Probe @("--pipe", $PipeName, "following", $artist, $album, $playlist,
        "spotify:album:nope00000000000000001", "spotify:artist:nope00000000000000000",
        "spotify:playlist:nope0000000000000001")
    Assert ($r.Code -eq 0 -and $r.Output -match "following true true true false false false") `
        "$PipeName following reports membership in request order"

    $r = Invoke-Probe @("--pipe", $PipeName, "unfollow", $artist)
    $r = Invoke-Probe @("--pipe", $PipeName, "following", $artist)
    Assert ($r.Output -match "following false") "$PipeName unfollow clears membership"
    $r = Invoke-Probe @("--pipe", $PipeName, "follow", $artist)
    $r = Invoke-Probe @("--pipe", $PipeName, "following", $artist)
    Assert ($r.Output -match "following true") "$PipeName follow restores membership"
    $r = Invoke-Probe @("--pipe", $PipeName, "follow", "spotify:track:0123456789abcdefghijkl")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=bad_request") `
        "$PipeName follow refuses a track"

    $r = Invoke-Probe @("--pipe", $PipeName, "playlist_create", "New Mix")
    $createdUri = $null
    if ($r.Output -match "playlist_create ok uri=(spotify:playlist:\S+)") { $createdUri = $Matches[1] }
    Assert ($r.Code -eq 0 -and $createdUri -ne $null -and $r.Output -match "revision=") `
        "$PipeName playlist_create returns the new item"
    $r = Invoke-Probe @("--pipe", $PipeName, "library")
    Assert ($r.Output -match "name=New Mix .*editable=true") `
        "$PipeName playlist_create adds the playlist to the library"

    $r = Invoke-Probe @("--pipe", $PipeName, "playlist_rename", $playlist, "Road Trip Renamed")
    Assert ($r.Code -eq 0 -and $r.Output -match "playlist_rename ok revision=") `
        "$PipeName playlist_rename succeeds"
    $r = Invoke-Probe @("--pipe", $PipeName, "library")
    Assert ($r.Output -match "name=Road Trip Renamed") "$PipeName rename shows in the library"

    $r = Invoke-Probe @("--pipe", $PipeName, "playlist_remove", $playlist,
        "spotify:track:plsecond00000000000001:1")
    Assert ($r.Code -eq 0 -and $r.Output -match "playlist_remove ok count=1 revision=") `
        "$PipeName playlist_remove succeeds"
    $r = Invoke-Probe @("--pipe", $PipeName, "playlist_remove", $playlist,
        "spotify:track:plsecond00000000000001:1")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=bad_request") `
        "$PipeName playlist_remove refuses a stale index"
    $r = Invoke-Probe @("--pipe", $PipeName, "playlist_remove", $playlist,
        "spotify:track:plthird000000000000001:0")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=bad_request") `
        "$PipeName playlist_remove refuses the wrong uri at an index"
    $r = Invoke-Probe @("--pipe", $PipeName, "playlist_remove", $playlist,
        "spotify:track:plfirst000000000000001:0")
    Assert ($r.Code -eq 0) "$PipeName playlist_remove removes the verified item"

    $r = Invoke-Probe @("--pipe", $PipeName, "playlist_delete", $createdUri)
    Assert ($r.Code -eq 0 -and $r.Output -match "playlist_delete ok") "$PipeName playlist_delete succeeds"
    $r = Invoke-Probe @("--pipe", $PipeName, "library")
    Assert ($r.Output -notmatch "name=New Mix") "$PipeName deleted playlist leaves the library"
}


# Stops only the bridges this script spawned: the fake copied to librespot-bridge.exe
# runs with a --pipe name starting with foo_librespot_spawn. Never stop by image name,
# the user's foobar2000 runs its own librespot-bridge.exe.
function Stop-TestBridges {
    Get-CimInstance Win32_Process -Filter "Name = 'librespot-bridge.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match '--pipe foo_librespot_spawn' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

$started = @()
try {
    # --- normal fake bridge ------------------------------------------------
    $normalPipe = "foo_librespot_test"
    $started += Start-Process -FilePath $fake `
        -ArgumentList @("--pipe", $normalPipe, "--duration-ms", "2000", "--quiet") -PassThru
    Wait-Pipe $normalPipe

    $r = Invoke-Probe @("--pipe", $normalPipe, "status")
    Assert ($r.Code -eq 0 -and $r.Output -match "logged_in=true") "status reports logged in"
    Assert ($r.Output -match "version=0.1.0-fake") "status carries bridge version"

    $r = Invoke-Probe @("--pipe", $normalPipe, "info", "spotify:track:0123456789abcdefghijkl")
    Assert ($r.Code -eq 0) "info returns success"
    Assert ($r.Output -match "title=Fake Track 1") "info title"
    Assert ($r.Output -match "artists=2") "info multi-artist"
    Assert ($r.Output -match "duration_ms=2000") "info duration"
    Assert ($r.Output -match "gain=yes" -and $r.Output -match "peak=yes") "info ReplayGain"

    $r = Invoke-Probe @("--pipe", $normalPipe, "resolve", "spotify:album:4aawyAB9vmqN3uQ7FjRGTy")
    Assert ($r.Code -eq 0 -and $r.Output -match "resolve count=3") "resolve album into 3 tracks"
    Assert ($r.Output -match "context uri=spotify:album:4aawyAB9vmqN3uQ7FjRGTy") "resolve carries album context"
    Assert ($r.Output -match "context .*revision=1") "resolve context revision"

    # --- protocol v2: library / search / artist / revision -----------------
    $r = Invoke-Probe @("--pipe", $normalPipe, "library")
    Assert ($r.Code -eq 0 -and $r.Output -match "library sections=8") "library has 8 sections"
    Assert ($r.Output -match "section id=liked .*items=1") "liked section"
    Assert ($r.Output -match "section id=made_for_you .*items=2") "made-for-you section"
    Assert ($r.Output -match "kind=collection name=Liked Songs") "Liked Songs collection item"
    Assert ($r.Output -match "kind=artist name=Fake Artist") "followed artist item"

    $r = Invoke-Probe @("--pipe", $normalPipe, "search", "nation of language", "--limit", "2")
    Assert ($r.Code -eq 0 -and $r.Output -match "search tracks=2 albums=1 artists=1 playlists=1") `
        "search returns every category"

    $r = Invoke-Probe @("--pipe", $normalPipe, "artist", "spotify:artist:art0000000000000000001")
    Assert ($r.Code -eq 0 -and $r.Output -match "artist top_tracks=2 albums=2 singles=1") `
        "artist page has top tracks, albums and singles"

    $r = Invoke-Probe @("--pipe", $normalPipe, "control", "--revision", "2", "--tracks", "4")
    Assert ($r.Code -eq 0 -and $r.Output -match "control revision=2 tracks=4") "test control changes state"

    $r = Invoke-Probe @("--pipe", $normalPipe, "resolve", "spotify:playlist:user000000000000000001")
    Assert ($r.Code -eq 0 -and $r.Output -match "resolve count=4") "revision change changes track count"
    Assert ($r.Output -match "context .*revision=2 .*track_count=4") "new context revision"

    # --- protocol v3: saved/save/unsave, playlist_add, radio, album/artist uris ----
    $trackA = "spotify:track:0123456789abcdefghijkl"
    $trackB = "spotify:track:zyxwvutsrqponmlkjihgfe"
    $r = Invoke-Probe @("--pipe", $normalPipe, "info", $trackA)
    Assert ($r.Output -match "album_uri=spotify:album:fakealbum000000000001 artist_uris=2") `
        "info carries optional album_uri and artist_uris"

    $r = Invoke-Probe @("--pipe", $normalPipe, "saved", $trackA, $trackB)
    Assert ($r.Code -eq 0 -and $r.Output -match "saved false false") "saved: nothing liked yet"
    $r = Invoke-Probe @("--pipe", $normalPipe, "save", $trackA)
    Assert ($r.Code -eq 0 -and $r.Output -match "save ok count=1") "save succeeds"
    $r = Invoke-Probe @("--pipe", $normalPipe, "saved", $trackA, $trackB)
    Assert ($r.Output -match "saved true false") "saved reflects save, same order"
    $r = Invoke-Probe @("--pipe", $normalPipe, "unsave", $trackA)
    $r = Invoke-Probe @("--pipe", $normalPipe, "saved", $trackA)
    Assert ($r.Output -match "saved false") "unsave removes the like"

    $r = Invoke-Probe @("--pipe", $normalPipe, "library")
    Assert ($r.Output -match "name=Road Trip .*editable=true") "editable playlist flag (true)"
    Assert ($r.Output -match "name=Shared Mix .*editable=false") "editable playlist flag (false)"
    Assert ($r.Output -match "name=Daily Mix 1 .*editable=false") "items without the flag default to false"

    $r = Invoke-Probe @("--pipe", $normalPipe, "playlist_add", "spotify:playlist:user000000000000000001", $trackA, $trackB)
    Assert ($r.Code -eq 0 -and $r.Output -match "playlist_add ok revision=pa1") "playlist_add returns a revision"
    $r = Invoke-Probe @("--pipe", $normalPipe, "playlist_add", "spotify:playlist:shared00000000000001", $trackA)
    Assert ($r.Code -eq 1 -and $r.Output -match "code=bad_request") "playlist_add to a foreign playlist is bad_request"

    $r = Invoke-Probe @("--pipe", $normalPipe, "radio", $trackA)
    Assert ($r.Code -eq 0 -and $r.Output -match "radio count=4") "radio returns tracks"
    Assert ($r.Output -match "context uri=spotify:station:.* name=Fake Track 1 Radio .*revision=r1") "radio carries a context"
    $r = Invoke-Probe @("--pipe", $normalPipe, "radio", "spotify:track:unsupported")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=unsupported") "unsupported writes surface as code=unsupported"

    $r = Invoke-Probe @("--pipe", $normalPipe, "info", "--bitrate", "96",
        "spotify:track:0123456789abcdefghijkl")
    Assert ($r.Code -eq 0 -and $r.Output -match "bitrate=96") "info reports the preferred bitrate"

    $r = Invoke-Probe @("--pipe", $normalPipe, "play", "spotify:track:0123456789abcdefghijkl",
        "--frames", "all", "--start-ms", "0", "--expect-start-frame", "0", "--expect-frames", "88200")
    Assert ($r.Code -eq 0) "play full 2 s track"
    Assert ($r.Output -match "frames=88200") "2 s at 44100 Hz = 88200 frames"
    Assert ($r.Output -match "peak=0.250000") "float32 PCM peak is 0.25"
    Assert ($r.Output -match "rms=0.176777") "float32 PCM RMS matches a 0.25 sine"
    Assert ($r.Output -match "channel_mismatch=no") "stereo interleaving is intact"

    $r = Invoke-Probe @("--pipe", $normalPipe, "play", "spotify:track:0123456789abcdefghijkl",
        "--start-ms", "1000", "--expect-start-frame", "44100", "--frames", "all",
        "--expect-frames", "44100")
    Assert ($r.Code -eq 0) "seek to 1 s"
    Assert ($r.Output -match "start_frame=44100") "seek header start_frame is sample-accurate"
    Assert ($r.Output -match "frames=44100") "seek delivers the remaining 1 s"

    # --- protocol v4: error codes, config/cache, album gain ----------------
    $r = Invoke-Probe @("--pipe", $normalPipe, "status")
    Assert ($r.Output -match "product=premium") "status carries the optional product"

    $r = Invoke-Probe @("--pipe", $normalPipe, "info", "spotify:track:errpremium00000000000")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=premium_required") "premium_required error code"
    $r = Invoke-Probe @("--pipe", $normalPipe, "info", "spotify:track:errrate000000000000000")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=rate_limited retry_after=42") "rate_limited carries retry_after"
    $r = Invoke-Probe @("--pipe", $normalPipe, "play", "spotify:track:errregion0000000000000")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=region_unavailable") "region_unavailable on open"
    $r = Invoke-Probe @("--pipe", $normalPipe, "play", "spotify:track:errgone00000000000000")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=track_unavailable") "track_unavailable on open"

    $r = Invoke-Probe @("--pipe", $normalPipe, "info", "spotify:track:albumgain0000000000000")
    Assert ($r.Output -match "album_gain=yes album_peak=yes") "info carries album gain and peak"
    $r = Invoke-Probe @("--pipe", $normalPipe, "info", "spotify:track:0123456789abcdefghijkl")
    Assert ($r.Output -match "album_gain=no album_peak=no") "missing album gain stays absent"

    $r = Invoke-Probe @("--pipe", $normalPipe, "config")
    Assert ($r.Code -eq 0 -and $r.Output -match "config cache_mb=0 ") "config reports the cache state"
    $r = Invoke-Probe @("--pipe", $normalPipe, "play", "spotify:track:albumgain0000000000000", "--frames", "4096")
    Assert ($r.Output -match "from_cache=no album_gain=yes") "open header: album gain, cache off -> not from cache"
    $r = Invoke-Probe @("--pipe", $normalPipe, "config", "--cache-mb", "1024")
    Assert ($r.Code -eq 0 -and $r.Output -match "config cache_mb=1024 ") "config applies the cache size live"
    $r = Invoke-Probe @("--pipe", $normalPipe, "play", "spotify:track:albumgain0000000000000", "--frames", "4096")
    Assert ($r.Output -match "from_cache=no ") "first open with the cache on comes from the network"
    $r = Invoke-Probe @("--pipe", $normalPipe, "play", "spotify:track:albumgain0000000000000", "--frames", "4096")
    Assert ($r.Output -match "from_cache=yes ") "second open reports from_cache"
    $r = Invoke-Probe @("--pipe", $normalPipe, "config", "--clear")
    Assert ($r.Output -match "cache_bytes=0") "clear_cache empties the cache"
    $r = Invoke-Probe @("--pipe", $normalPipe, "play", "spotify:track:albumgain0000000000000", "--frames", "4096")
    Assert ($r.Output -match "from_cache=no ") "after clearing, the next open is a miss"
    $r = Invoke-Probe @("--pipe", $normalPipe, "config", "--cache-mb", "0")
    Assert ($r.Output -match "config cache_mb=0 ") "cache can be turned off live"

    # --- protocol v5: following, playlist management, new sections ---------
    Assert-ManagementOps $normalPipe

    # --- abort test (real-time stream) ------------------------------------
    $realtimePipe = "foo_librespot_rt"
    $started += Start-Process -FilePath $fake `
        -ArgumentList @("--pipe", $realtimePipe, "--duration-ms", "60000", "--realtime", "--quiet") -PassThru
    Wait-Pipe $realtimePipe

    $r = Invoke-Probe @("--pipe", $realtimePipe, "play", "spotify:track:0123456789abcdefghijkl",
        "--frames", "all", "--abort-after-ms", "300")
    Assert ($r.Code -eq 3) "abort_callback cancels playback (exit 3)"
    Assert ($r.Output -match "cancelled") "cancellation is reported"
    if ($r.Output -match "after_ms=(\d+)") {
        Assert ([int]$Matches[1] -lt 2000) "cancellation latency under 2 s"
    } else {
        Assert $false "cancellation latency reported"
    }

    $r = Invoke-Probe @("--pipe", $realtimePipe, "status")
    Assert ($r.Code -eq 0) "bridge survives a client disconnect mid-track"

    # --- not logged in -----------------------------------------------------
    $loggedOutPipe = "foo_librespot_out"
    $started += Start-Process -FilePath $fake `
        -ArgumentList @("--pipe", $loggedOutPipe, "--duration-ms", "500", "--logged-out", "--quiet") -PassThru
    Wait-Pipe $loggedOutPipe

    $r = Invoke-Probe @("--pipe", $loggedOutPipe, "info", "spotify:track:0123456789abcdefghijkl")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=not_logged_in") "not_logged_in error code"
    $r = Invoke-Probe @("--pipe", $loggedOutPipe, "save", "spotify:track:0123456789abcdefghijkl")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=not_logged_in") "v3 writes need a login too"

    # --- protocol v6: owner, liked, play reports, debug, logout ------------
    $v6Pipe = "foo_librespot_v6"
    $started += Start-Process -FilePath $fake `
        -ArgumentList @("--pipe", $v6Pipe, "--duration-ms", "500", "--parent-pid", "$PID", "--quiet") -PassThru
    Wait-Pipe $v6Pipe
    $r = Invoke-Probe @("--pipe", $v6Pipe, "status")
    Assert ($r.Output -match "parent_pid=$PID " -and $r.Output -match "pid=\d+ ") "v6 status reports pid and parent_pid"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "save", "spotify:track:liked000000000000000001")
    $r = Invoke-Probe @("--pipe", $v6Pipe, "liked")
    Assert ($r.Code -eq 0 -and $r.Output -match "liked count=1" -and
        $r.Output -match "liked uri=spotify:track:liked000000000000000001") "liked lists the saved tracks"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "report_play", "spotify:track:liked000000000000000001", "--ms", "45000")
    Assert ($r.Code -eq 0 -and $r.Output -match "report_play ok") "report_play accepted"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "report_play", "spotify:album:alb0000000000000000001")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=bad_request") "report_play refuses a non-track"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "control")
    Assert ($r.Output -match "play_reports=1") "one play recorded, the refused one not"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "config", "--debug", "on")
    Assert ($r.Code -eq 0 -and $r.Output -match "config debug=true") "config switches debug logging on"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "status")
    Assert ($r.Output -match "debug=true") "status reports debug logging"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "shutdown")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=bad_request") "shutdown refused for a process that does not own the bridge"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "status")
    Assert ($r.Code -eq 0) "bridge keeps running after a refused shutdown"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "logout")
    Assert ($r.Code -eq 0 -and $r.Output -match "logout ok") "logout succeeds"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "status")
    Assert ($r.Output -match "logged_in=false") "status after logout is logged out"
    $r = Invoke-Probe @("--pipe", $v6Pipe, "liked")
    Assert ($r.Code -eq 1 -and $r.Output -match "code=not_logged_in") "liked needs a login"

    # --- playback rules (truncation, play counting, rate limiter) -----------
    $r = Invoke-Probe @("rules")
    if ($r.Code -ne 0) { Write-Host $r.Output.Trim() }
    Assert ($r.Code -eq 0 -and $r.Output -match "rules failures=0") "playback rules self-test"

    # --- metadata mapping (SDK file_info, no host) -------------------------
    $sharedDll = if ($Foobar2000Dir) { Join-Path $Foobar2000Dir "shared.dll" } else { "" }
    if ($sharedDll -and (Test-Path $sharedDll)) {
        # The probe must match the architecture of the foobar2000 install it borrows
        # shared.dll from (the portable install may be 32-bit or 64-bit).
        $dllBytes = [IO.File]::ReadAllBytes($sharedDll)
        $machine = [BitConverter]::ToUInt16($dllBytes, [BitConverter]::ToInt32($dllBytes, 0x3c) + 4)
        $arch = if ($machine -eq 0x8664) { "x64" } else { "Win32" }
        $metadataProbe = Join-Path $componentDir "build\tools\$Configuration\$arch\metadata_probe.exe"
        Copy-Item $sharedDll (Split-Path $metadataProbe) -Force
        $metadataOutput = & $metadataProbe 2>&1 | Out-String
        $metadataCode = $LASTEXITCODE
        Write-Host $metadataOutput.Trim()
        Assert ($metadataCode -eq 0 -and $metadataOutput -notmatch "FAIL") `
            "metadata mapping (title/artist/album/track/disc/date/codec/bitrate/ReplayGain/SOURCE)"
    } else {
        Write-Host "SKIP  metadata mapping test (shared.dll not found under $Foobar2000Dir)"
    }

    # --- sync plan (pure functions, no host) --------------------------------
    $syncOutput = & $syncPlanTest 2>&1 | Out-String
    $syncCode = $LASTEXITCODE
    Assert ($syncCode -eq 0 -and $syncOutput -match "All sync plan tests passed") `
        "sync plan (insert/remove/reorder/duplicates/protected playing item), insert runs, Spotify link parsing"

    # --- process spawn -----------------------------------------------------
    $spawnDir = Join-Path $env:TEMP "foo_librespot_spawn_test"
    New-Item -ItemType Directory -Force $spawnDir | Out-Null
    Copy-Item $fake (Join-Path $spawnDir "librespot-bridge.exe") -Force
    $spawnPipe = "foo_librespot_spawn"
    $r = Invoke-Probe @("--pipe", $spawnPipe, "spawn", $spawnDir)
    Assert ($r.Code -eq 0 -and $r.Output -match "spawn ok") "bridge_process starts librespot-bridge.exe"
    Assert ($r.Output -match "logged_in=true") "spawned bridge answers status"

    # The cache size reaches a spawned bridge as --cache-mb.
    Stop-TestBridges
    $r = Invoke-Probe @("--pipe", "foo_librespot_spawn2", "spawn", $spawnDir, "--cache-mb", "512")
    Assert ($r.Code -eq 0 -and $r.Output -match "config cache_mb=512 ") "spawned bridge receives --cache-mb"
    Stop-TestBridges

    # Quit: the bridge this process spawned is shut down (owner-only op).
    $r = Invoke-Probe @("--pipe", "foo_librespot_spawn3", "spawn", $spawnDir, "--debug", "--stop")
    Assert ($r.Output -match "debug=true") "spawned bridge receives --debug"
    Assert ($r.Output -match "owned=true") "a spawned bridge is owned"
    Assert ($r.Code -eq 0 -and $r.Output -match "stop result=stopped pipe_reachable=false") "stop_owned shuts down the spawned bridge"
    Assert ($r.Output -match "restart_refused=true owned_after=false") "no bridge is started again after the final stop"
    Stop-TestBridges

    # A bridge another process owns is used but never stopped; a version
    # mismatch is reported once.
    $foreignPipe = "foo_librespot_foreign"
    $foreign = Start-Process -FilePath $fake -PassThru `
        -ArgumentList @("--pipe", $foreignPipe, "--parent-pid", "$PID", "--version", "9.9.9", "--quiet")
    $started += $foreign
    Wait-Pipe $foreignPipe
    $r = Invoke-Probe @("--pipe", $foreignPipe, "spawn", $spawnDir, "--expect-version", "0.4.0", "--stop")
    Assert ($r.Output -match "warning: .*version 9\.9\.9") "version mismatch with a foreign bridge is reported"
    Assert ($r.Output -match "owned=false" -and $r.Output -match "stop result=not_ours pipe_reachable=true") `
        "a foreign bridge is not stopped"
    Assert (-not $foreign.HasExited) "the foreign bridge process keeps running"

    # A leftover whose foobar2000 has exited: waited for, then reported.
    $deadPid = 999999
    while (Get-Process -Id $deadPid -ErrorAction SilentlyContinue) { $deadPid++ }
    $leftoverPipe = "foo_librespot_leftover"
    $started += Start-Process -FilePath $fake -PassThru `
        -ArgumentList @("--pipe", $leftoverPipe, "--parent-pid", "$deadPid", "--quiet")
    Wait-Pipe $leftoverPipe
    $r = Invoke-Probe @("--pipe", $leftoverPipe, "spawn", $spawnDir, "--stop")
    Assert ($r.Output -match "warning: .*left over" -and $r.Output -match "stop result=not_ours") `
        "a leftover bridge of an exited foobar2000 is reported, not killed"

    # --- Python fake bridge ------------------------------------------------
    $pythonCandidates = @(
        "python.exe", "py.exe"
    )
    $python = $pythonCandidates | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } |
        Select-Object -First 1
    if ($python) {
        & $python -c "import win32pipe" 2> $null
        if ($LASTEXITCODE -eq 0) {
            $pythonPipe = "foo_librespot_py"
            $started += Start-Process -FilePath $python `
                -ArgumentList @((Join-Path $componentDir "tools\fake_bridge.py"), "--pipe", $pythonPipe,
                    "--duration-ms", "1000", "--quiet") -PassThru
            Wait-Pipe $pythonPipe
            $r = Invoke-Probe @("--pipe", $pythonPipe, "status")
            Assert ($r.Code -eq 0 -and $r.Output -match "logged_in=true") "python fake bridge status"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "play", "spotify:track:0123456789abcdefghijkl",
                "--frames", "all", "--expect-frames", "44100")
            Assert ($r.Code -eq 0 -and $r.Output -match "peak=0.250000") "python fake bridge PCM"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "library")
            Assert ($r.Code -eq 0 -and $r.Output -match "library sections=8" `
                -and $r.Output -match "kind=collection") "python fake bridge library"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "saved", "spotify:track:0123456789abcdefghijkl")
            Assert ($r.Code -eq 0 -and $r.Output -match "saved false") "python fake bridge saved"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "save", "spotify:track:0123456789abcdefghijkl")
            $r = Invoke-Probe @("--pipe", $pythonPipe, "saved", "spotify:track:0123456789abcdefghijkl")
            Assert ($r.Output -match "saved true") "python fake bridge save/saved round trip"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "library")
            Assert ($r.Output -match "name=Road Trip .*editable=true") "python fake bridge editable playlists"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "playlist_add", "spotify:playlist:user000000000000000001", "spotify:track:0123456789abcdefghijkl")
            Assert ($r.Code -eq 0 -and $r.Output -match "revision=pa1") "python fake bridge playlist_add"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "radio", "spotify:track:0123456789abcdefghijkl")
            Assert ($r.Code -eq 0 -and $r.Output -match "radio count=4" -and $r.Output -match "name=Fake Track 1 Radio") "python fake bridge radio"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "info", "spotify:track:0123456789abcdefghijkl")
            Assert ($r.Output -match "album_uri=spotify:album:fakealbum000000000001") "python fake bridge album_uri"
            $r = Invoke-Probe @("--pipe", $pythonPipe, "resolve", "spotify:playlist:py0000000000000000001")
            Assert ($r.Code -eq 0 -and $r.Output -match "context .*revision=\d+") `
                "python fake bridge resolve context"
            Assert-ManagementOps $pythonPipe
        } else {
            Write-Host "SKIP  python fake bridge (pywin32 not installed)"
        }
    } else {
        Write-Host "SKIP  python fake bridge (python not found)"
    }

} finally {
    foreach ($process in $started) {
        if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    Stop-TestBridges
}

if ($failures -ne 0) {
    Write-Host "$failures test(s) FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "All tests passed" -ForegroundColor Green
