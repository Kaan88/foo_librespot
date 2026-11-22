# Builds Release x64 and produces dist/foo_librespot.fb2k-component.
#
# foobar2000 v2 component package layout for an x64 component:
#   x64/foo_librespot.dll
#   x64/librespot-bridge.exe
# (the DLL and its helper stay side by side so core_api::get_my_full_path()
# based discovery finds the bridge).
#
#   .\scripts\package.ps1
#   .\scripts\package.ps1 -BridgeExe ..\bridge\bin\librespot-bridge.exe
#   .\scripts\package.ps1 -SkipBuild

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$BridgeExe = (Join-Path $PSScriptRoot "..\..\bridge\bin\librespot-bridge.exe"),
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$componentDir = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $componentDir "build\$Configuration\x64\foo_librespot.dll"
$distDir = Join-Path $componentDir "dist"
$stage = Join-Path $componentDir "build\package"
$zip = Join-Path $distDir "foo_librespot.fb2k-component"

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -Platform x64
}

if (-not (Test-Path $dll)) { throw "component DLL not found: $dll (run build.ps1 first)" }

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$x64 = Join-Path $stage "x64"
New-Item -ItemType Directory -Force $x64 | Out-Null
Copy-Item $dll (Join-Path $x64 "foo_librespot.dll")

$resolvedBridge = $null
if ($BridgeExe -and (Test-Path $BridgeExe)) {
    $resolvedBridge = (Resolve-Path $BridgeExe).Path
    Copy-Item $resolvedBridge (Join-Path $x64 "librespot-bridge.exe")
} else {
    Write-Warning "librespot-bridge.exe not found at '$BridgeExe'; packaging the DLL only."
    Write-Warning "Pass -BridgeExe <path> to include the helper."
}

New-Item -ItemType Directory -Force $distDir | Out-Null
if (Test-Path $zip) { Remove-Item -Force $zip }

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $stage,
    $zip,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false)

Write-Host "Created $zip"
Write-Host "Contents:"
Add-Type -AssemblyName System.IO.Compression
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($entry in $archive.Entries) {
        Write-Host ("  {0}  ({1} bytes)" -f $entry.FullName, $entry.Length)
    }
} finally {
    $archive.Dispose()
}

if (-not $resolvedBridge) {
    Write-Warning "Package does not contain librespot-bridge.exe; it will not work until the helper is supplied."
}
