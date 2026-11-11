param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
dotnet build Samples/Managed/SystemBenchmark/SystemBenchmark.csproj --configuration $Configuration --nologo
if($LASTEXITCODE -ne 0){throw 'SystemBenchmark build failed.'}
$native=Join-Path $projectRoot ('out/build/windows-x64-'+$Configuration.ToLowerInvariant())
$destination=Join-Path $projectRoot ('out/managed/SystemBenchmark/'+$Configuration+'/net10.0')
Copy-Item -LiteralPath (Join-Path $native 'EngineNative.dll') -Destination $destination -Force
. (Join-Path $PSScriptRoot 'Copy-RendererResources.ps1')
$resources=Join-Path $destination 'Resources'
New-Item -ItemType Directory -Path $resources -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'Resources/Localization') -Destination $resources -Recurse -Force
Copy-EngineRendererResources -NativeDirectory $native -Destination $resources
