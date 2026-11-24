# Fetches the Columns UI SDK into component/third_party/foobar2000_sdk/columns_ui-sdk.
#
#   SDK: https://github.com/reupen/columns_ui-sdk (tagged release archive)
#   Installed layout: third_party/foobar2000_sdk/columns_ui-sdk/<repository files>
#
# Its headers include "../pfc/pfc.h" and "../foobar2000/SDK/...", so it has to
# sit next to the foobar2000 SDK's pfc/ and foobar2000/ folders. Run
# fetch_sdk.ps1 first (build.ps1 calls both). Not committed, like the
# foobar2000 SDK.

param(
    [string]$Version = "8.1.0",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$componentDir = Split-Path -Parent $PSScriptRoot
$sdkRoot = Join-Path $componentDir "third_party\foobar2000_sdk"
$dest = Join-Path $sdkRoot "columns_ui-sdk"
$marker = Join-Path $dest "columns_ui-sdk-public.vcxproj"
$versionFile = Join-Path $dest ".fetched-version"

if (-not (Test-Path (Join-Path $sdkRoot "pfc\pfc.h"))) {
    throw "The foobar2000 SDK is missing; run scripts\fetch_sdk.ps1 first"
}

if ((Test-Path $marker) -and (Test-Path $versionFile) -and -not $Force) {
    $present = (Get-Content $versionFile -Raw).Trim()
    if ($present -eq $Version) {
        Write-Host "Columns UI SDK $Version already present at $dest"
        exit 0
    }
}

$url = "https://github.com/reupen/columns_ui-sdk/archive/refs/tags/v$Version.zip"
$archive = Join-Path $env:TEMP "columns_ui-sdk-$Version.zip"
$staging = Join-Path $env:TEMP "columns_ui-sdk-$Version"

Write-Host "Downloading $url"
if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
    & curl.exe -L --fail --silent --show-error -o $archive $url
    if ($LASTEXITCODE -ne 0) { throw "curl failed with exit code $LASTEXITCODE" }
} else {
    Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
}

if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging | Out-Null
Expand-Archive -Path $archive -DestinationPath $staging -Force

# GitHub archives wrap the files in one "columns_ui-sdk-<version>" folder.
$inner = Get-ChildItem $staging -Directory | Select-Object -First 1
if (-not $inner -or -not (Test-Path (Join-Path $inner.FullName "columns_ui-sdk-public.vcxproj"))) {
    throw "Unexpected Columns UI SDK archive layout in $staging"
}

if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Move-Item $inner.FullName $dest
Set-Content -Path $versionFile -Value $Version -Encoding ASCII

Remove-Item -Recurse -Force $staging
Remove-Item -Force $archive
Write-Host "Columns UI SDK $Version installed to $dest"
