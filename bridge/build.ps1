# Builds bridge/bin/spike.exe with the MinGW-w64 decode libraries.
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

# Link the C decode libraries statically so spike.exe needs no MinGW DLLs.
# - FLAC__NO_DLL: FLAC headers otherwise declare dllimport when libFLAC.a is
#   linked; without it the link fails on __imp_FLAC__* symbols.
# - -lshlwapi: mpg123's static archives use PathIsRelativeW/PathCombineW.
$env:CGO_CFLAGS = "-DFLAC__NO_DLL"
$env:CGO_LDFLAGS = "-lshlwapi"

Push-Location $root
try {
    New-Item -ItemType Directory -Force (Join-Path $root "bin") | Out-Null
    & $go build -trimpath -ldflags "-linkmode external -extldflags -static" -o "bin\spike.exe" ./cmd/spike
    if ($LASTEXITCODE -ne 0) {
        throw "go build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

$exe = Join-Path $root "bin\spike.exe"
$size = (Get-Item $exe).Length
Write-Host "Built $exe ($([math]::Round($size / 1MB, 1)) MiB)"
