# Builds foo_librespot and (optionally) the test tools with MSBuild.
#
#   .\scripts\build.ps1                      # Release x64
#   .\scripts\build.ps1 -Configuration Debug -Platform x64
#   .\scripts\build.ps1 -WithTools           # also build tools/fake_bridge, bridge_probe, metadata_probe, sync_plan_test
#
# The version of the SDK projects is retargeted to the locally installed
# platform toolset with /p:PlatformToolset (VS 2026 = v145), without editing
# the SDK files.

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateSet("Win32", "x64", "ARM64")]
    [string]$Platform = "x64",
    [string]$PlatformToolset = "",
    [switch]$WithTools,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$componentDir = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot "fetch_sdk.ps1")
& (Join-Path $PSScriptRoot "fetch_cui_sdk.ps1")

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio C++ tools" }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { $vsPath = & $vswhere -latest -property installationPath }
if (-not $vsPath) { throw "Visual Studio installation not found" }

$msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) { throw "MSBuild not found at $msbuild" }

if (-not $PlatformToolset) {
    $msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
    $latest = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($latest) {
        $parts = $latest.Name.Split(".")
        $key = "$($parts[0]).$($parts[1])"
        switch ($key) {
            "14.5" { $PlatformToolset = "v145" }
            "14.51" { $PlatformToolset = "v145" }
            "14.4" { $PlatformToolset = "v143" }
            "14.3" { $PlatformToolset = "v143" }
            default { $PlatformToolset = "v143" }
        }
    } else {
        $PlatformToolset = "v143"
    }
}

function Invoke-MSBuild([string]$project) {
    Write-Host "Building $project ($Configuration|$Platform, toolset $PlatformToolset)"
    $arguments = @(
        $project,
        "/m",
        "/nologo",
        "/v:minimal",
        "/p:Configuration=$Configuration",
        "/p:Platform=$Platform",
        "/p:PlatformToolset=$PlatformToolset"
    )
    if ($Clean) { $arguments += "/t:Rebuild" }
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed for $project (exit code $LASTEXITCODE)" }
}

Invoke-MSBuild (Join-Path $componentDir "foo_librespot.vcxproj")

if ($WithTools) {
    Invoke-MSBuild (Join-Path $componentDir "tools\fake_bridge\fake_bridge.vcxproj")
    Invoke-MSBuild (Join-Path $componentDir "tools\bridge_probe\bridge_probe.vcxproj")
    Invoke-MSBuild (Join-Path $componentDir "tools\metadata_probe\metadata_probe.vcxproj")
    Invoke-MSBuild (Join-Path $componentDir "tools\sync_plan_test\sync_plan_test.vcxproj")
}

Write-Host "Build OK: $(Join-Path $componentDir "build\$Configuration\$Platform\foo_librespot.dll")"
