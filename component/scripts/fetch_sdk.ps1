# Fetches the official foobar2000 SDK into component/third_party/foobar2000_sdk.
#
#   SDK: https://www.foobar2000.org/SDK
#   Archive layout: foobar2000/, pfc/, libPPUI/, sdk-license.txt, sdk-readme.html
#   Installed layout: third_party/foobar2000_sdk/<those entries>
#
# The SDK is intentionally not committed; rerun this script (or build.ps1, which
# calls it) to restore it.

param(
    [string]$Version = "2026-10-01",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$componentDir = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $componentDir "third_party\foobar2000_sdk"
$marker = Join-Path $dest "foobar2000\SDK\foobar2000_SDK.vcxproj"

if ((Test-Path $marker) -and -not $Force) {
    Write-Host "foobar2000 SDK $Version already present at $dest"
    exit 0
}

$url = "https://www.foobar2000.org/downloads/SDK-$Version.7z"
$archive = Join-Path $env:TEMP "foobar2000-SDK-$Version.7z"
$staging = Join-Path $env:TEMP "foobar2000-SDK-$Version"

Write-Host "Downloading $url"
if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
    & curl.exe -L --fail --silent --show-error -o $archive $url
    if ($LASTEXITCODE -ne 0) { throw "curl failed with exit code $LASTEXITCODE" }
} else {
    Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
}

if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging | Out-Null

$sevenZipCandidates = @(
    "C:\Program Files\7-Zip\7z.exe",
    "C:\Program Files\7-Zip-Zstandard\7z.exe",
    "C:\Program Files (x86)\7-Zip\7z.exe"
)
$sevenZip = $sevenZipCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($sevenZip) {
    Write-Host "Extracting with $sevenZip"
    & $sevenZip x -y "-o$staging" $archive | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z extraction failed with exit code $LASTEXITCODE" }
} else {
    # Windows' bsdtar (libarchive) reads 7z archives.
    Write-Host "Extracting with tar.exe (bsdtar)"
    & tar.exe -xf $archive -C $staging
    if ($LASTEXITCODE -ne 0) { throw "tar extraction failed with exit code $LASTEXITCODE" }
}

if (-not (Test-Path (Join-Path $staging "foobar2000\SDK\foobar2000_SDK.vcxproj"))) {
    throw "Unexpected SDK archive layout; expected foobar2000/SDK in $staging"
}

if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force $dest | Out-Null

foreach ($entry in @("foobar2000", "pfc", "libPPUI", "sdk-license.txt", "sdk-readme.html")) {
    $src = Join-Path $staging $entry
    if (Test-Path $src) { Move-Item $src $dest }
}

Remove-Item -Recurse -Force $staging
Write-Host "foobar2000 SDK $Version installed to $dest"
