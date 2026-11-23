# Builds Release x64 and produces dist/foo_librespot.fb2k-component.
#
# The component is x64-only: 32-bit foobar2000 is no longer supported.
# Package layout for an x64-only foobar2000 v2 component:
#   x64/foo_librespot.dll      x64 DLL, loaded by 64-bit foobar2000
#   x64/librespot-bridge.exe   its helper, next to the DLL
#
# The component looks for librespot-bridge.exe next to its own DLL and then in
# the parent folder.
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
$x64Dll = Join-Path $componentDir "build\$Configuration\x64\foo_librespot.dll"
$distDir = Join-Path $componentDir "dist"
$stage = Join-Path $componentDir "build\package"
$zip = Join-Path $distDir "foo_librespot.fb2k-component"

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -Platform x64
}

if (-not (Test-Path $x64Dll)) { throw "component DLL not found: $x64Dll (run build.ps1 -Platform x64 first)" }

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$x64Stage = Join-Path $stage "x64"
New-Item -ItemType Directory -Force $x64Stage | Out-Null

Copy-Item $x64Dll (Join-Path $x64Stage "foo_librespot.dll")

$resolvedBridge = $null
if ($BridgeExe -and (Test-Path $BridgeExe)) {
    $resolvedBridge = (Resolve-Path $BridgeExe).Path
    Copy-Item $resolvedBridge (Join-Path $x64Stage "librespot-bridge.exe")
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
