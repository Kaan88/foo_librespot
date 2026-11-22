# Builds the bridge executables with the MinGW-w64 decode libraries:
#   bin\librespot-bridge.exe  named-pipe bridge (proto/PROTOCOL.md)
#   bin\spike.exe             Phase 0 login/dump tool
#   bin\bridgectl.exe         manual protocol client
#
# Requires:
#   - Go at C:\Program Files\Go\bin (not necessarily on PATH)
#   - MSYS2 MinGW-w64 gcc, pkg-config, libogg/libvorbis/libFLAC/mpg123 at
#     C:\msys64\mingw64 (override with -MsysRoot).
param(
    [string]$MsysRoot = "C:\msys64\mingw64",
    [string]$GoRoot = "C:\Program Files\Go"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$go = Join-Path $GoRoot "bin\go.exe"
if (-not (Test-Path $go)) {
    throw "go.exe not found at $go; pass -GoRoot or install Go"
}
if (-not (Test-Path (Join-Path $MsysRoot "bin\gcc.exe"))) {
    throw "gcc.exe not found under $MsysRoot; pass -MsysRoot or install MSYS2 MinGW-w64"
}

$env:PATH = "$(Join-Path $GoRoot 'bin');$(Join-Path $MsysRoot 'bin');$env:PATH"
$env:CGO_ENABLED = "1"
$env:CC = "gcc"
$pkgConfig = Join-Path $MsysRoot "lib\pkgconfig"
if ($env:PKG_CONFIG_PATH) {
    $env:PKG_CONFIG_PATH = "$pkgConfig;$env:PKG_CONFIG_PATH"
} else {
    $env:PKG_CONFIG_PATH = $pkgConfig
}

# Link the C decode libraries statically so the exes need no MinGW DLLs.
# - FLAC__NO_DLL: FLAC headers otherwise declare dllimport when libFLAC.a is
#   linked; without it the link fails on __imp_FLAC__* symbols.
# - -lshlwapi: mpg123's static archives use PathIsRelativeW/PathCombineW.
$env:CGO_CFLAGS = "-DFLAC__NO_DLL"
$env:CGO_LDFLAGS = "-lshlwapi"

$targets = @(
    @{ Out = "bin\librespot-bridge.exe"; Pkg = "./cmd/bridge" },
    @{ Out = "bin\spike.exe"; Pkg = "./cmd/spike" },
    @{ Out = "bin\bridgectl.exe"; Pkg = "./cmd/bridgectl" }
)

Push-Location $root
try {
    New-Item -ItemType Directory -Force (Join-Path $root "bin") | Out-Null
    foreach ($target in $targets) {
        & $go build -trimpath -ldflags "-linkmode external -extldflags -static" -o $target.Out $target.Pkg
        if ($LASTEXITCODE -ne 0) {
            throw "go build $($target.Pkg) failed with exit code $LASTEXITCODE"
        }
    }
} finally {
    Pop-Location
}

foreach ($target in $targets) {
    $exe = Join-Path $root $target.Out
    $size = (Get-Item $exe).Length
    Write-Host "Built $exe ($([math]::Round($size / 1MB, 1)) MiB)"
}
