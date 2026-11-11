param([string]$OutputDirectory='out/packages/graphics-g2', [switch]$SkipBuild)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $projectRoot
$packagePath=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory}else{Join-Path $projectRoot $OutputDirectory}))
if(-not $packagePath.StartsWith($projectRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Demo package must stay inside the workspace.'}
if(Test-Path -LiteralPath $packagePath){throw 'Use a new package directory; existing output is preserved.'}
if(-not $SkipBuild){
    & (Join-Path $PSScriptRoot 'Build-Test.ps1') -Configuration Release
    if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
}
$binaryDirectory=Join-Path $projectRoot 'out/build/windows-x64-release'
$executable=Join-Path $binaryDirectory 'EngineSandbox.exe'
if(-not (Test-Path -LiteralPath $executable)){throw 'Build the Release Player first.'}
New-Item -ItemType Directory -Path $packagePath | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $packagePath 'EngineSandbox.exe')
$resources=Join-Path $packagePath 'Resources'
New-Item -ItemType Directory -Path $resources | Out-Null
foreach($directory in @('Localization','Shaders','Materials','Demos')){
    Copy-Item -LiteralPath (Join-Path $binaryDirectory ('Resources/'+$directory)) -Destination $resources -Recurse
}
$environments=Join-Path $resources 'Environments'
New-Item -ItemType Directory -Path $environments | Out-Null
foreach($name in @('soft','contrast')){
    foreach($suffix in @('.seenv','.seenv.asset.json')){
        Copy-Item -LiteralPath (Join-Path $binaryDirectory ('Resources/Environments/'+$name+$suffix)) -Destination $environments
    }
}
$licenses=Join-Path $packagePath 'Licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
foreach($entry in @(@('cgltf','LICENSE'),@('ufbx','LICENSE'),@('nlohmann','LICENSE.MIT'),@('imgui','LICENSE.txt'))){
    Copy-Item -LiteralPath (Join-Path $projectRoot ('ThirdParty/'+$entry[0]+'/'+$entry[1])) -Destination (Join-Path $licenses ($entry[0]+'-'+$entry[1]))
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'Samples/Gallery/Package-README.txt') -Destination (Join-Path $packagePath 'README.txt')
foreach($name in @('materials','shadows','postprocess')){
    $launcher='@echo off'+"`r`n"+'cd /d "%~dp0"'+"`r`n"+'"%~dp0EngineSandbox.exe" --demo '+$name+"`r`n"
    [IO.File]::WriteAllText((Join-Path $packagePath ($name+'.cmd')),$launcher,[Text.ASCIIEncoding]::new())
}
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
$sourceManifest=Get-EngineSourceIdentity $projectRoot
Write-EngineJson (Join-Path $packagePath 'source-inputs.json') $sourceManifest
$runtimeFiles=@(Get-ChildItem -LiteralPath $packagePath -File -Recurse | Sort-Object FullName | ForEach-Object{
    [ordered]@{path=$_.FullName.Substring($packagePath.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$manifest=[ordered]@{schema=1;configuration='Release';git_head=$sourceManifest.git_head;dirty=$sourceManifest.dirty;source_sha256=$sourceManifest.source_sha256;runtime='Windows x64, D3D12 FL11_0/SM5_1, installed VC++ x64 runtime';source_hdr_required=$false;files=$runtimeFiles}
Write-EngineJson (Join-Path $packagePath 'manifest.json') $manifest
Write-Output ('Graphics demo package: '+$packagePath)
Write-Output ('Source content SHA-256: '+$sourceManifest.source_sha256)
exit 0
