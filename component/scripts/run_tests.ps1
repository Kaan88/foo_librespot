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
    [string]$Foobar2000Dir = "C:\Users\you\Desktop\foobar2000"
)

$ErrorActionPreference = "Stop"

$componentDir = Split-Path -Parent $PSScriptRoot
$probe = Join-Path $componentDir "build\tools\$Configuration\x64\bridge_probe.exe"
$fake = Join-Path $componentDir "build\tools\$Configuration\x64\fake_bridge.exe"
$metadataProbe = Join-Path $componentDir "build\tools\$Configuration\x64\metadata_probe.exe"

& (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -WithTools

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

    # --- metadata mapping (SDK file_info, no host) -------------------------
    $sharedDll = Join-Path $Foobar2000Dir "shared.dll"
    if (Test-Path $sharedDll) {
        Copy-Item $sharedDll (Split-Path $metadataProbe) -Force
        $metadataOutput = & $metadataProbe 2>&1 | Out-String
        $metadataCode = $LASTEXITCODE
        Write-Host $metadataOutput.Trim()
        Assert ($metadataCode -eq 0 -and $metadataOutput -notmatch "FAIL") `
            "metadata mapping (title/artist/album/track/disc/date/codec/bitrate/ReplayGain)"
    } else {
        Write-Host "SKIP  metadata mapping test (shared.dll not found under $Foobar2000Dir)"
    }

    # --- process spawn -----------------------------------------------------
    $spawnDir = Join-Path $env:TEMP "foo_librespot_spawn_test"
    New-Item -ItemType Directory -Force $spawnDir | Out-Null
    Copy-Item $fake (Join-Path $spawnDir "librespot-bridge.exe") -Force
    $spawnPipe = "foo_librespot_spawn"
    $r = Invoke-Probe @("--pipe", $spawnPipe, "spawn", $spawnDir)
    Assert ($r.Code -eq 0 -and $r.Output -match "spawn ok") "bridge_process starts librespot-bridge.exe"
    Assert ($r.Output -match "logged_in=true") "spawned bridge answers status"
    Get-Process -Name "librespot-bridge" -ErrorAction SilentlyContinue | Stop-Process -Force

    # --- Python fake bridge ------------------------------------------------
    $pythonCandidates = @(
        "C:\Users\you\AppData\Local\Programs\Python\Python313\python.exe",
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
    Get-Process -Name "librespot-bridge" -ErrorAction SilentlyContinue | Stop-Process -Force
}

if ($failures -ne 0) {
    Write-Host "$failures test(s) FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "All tests passed" -ForegroundColor Green
