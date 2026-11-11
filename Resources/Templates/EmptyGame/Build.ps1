param([Parameter(Mandatory=$true)][string]$SelfEngineRoot,[string]$Name='')
$ErrorActionPreference='Stop'
& powershell -NoProfile -File (Join-Path $SelfEngineRoot 'Scripts/Export-Game.ps1') -Project (Join-Path $PSScriptRoot 'project.json') -BuildSettings (Join-Path $PSScriptRoot 'build.json') -Name $Name
exit $LASTEXITCODE
