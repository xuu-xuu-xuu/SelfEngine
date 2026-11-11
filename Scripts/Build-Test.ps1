param([ValidateSet('Debug','Release')][string]$Configuration='Debug', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$env:TEMP = Join-Path $root 'out/tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null
. (Join-Path $PSScriptRoot 'Import-MsvcEnvironment.ps1')
Import-EngineMsvcEnvironment
$preset = 'windows-x64-' + $Configuration.ToLowerInvariant()
cmake --preset $preset
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
if (-not $SkipTests) {
    ctest --test-dir (Join-Path $root "out/build/$preset") --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
    & (Join-Path $PSScriptRoot 'Check-Localization.ps1')
}
