param([string]$OutputDirectory='out/packages/PortfolioCandidate',[string]$EvidenceDirectory='out/validation/continuous-20261101-c39dc9ef',[switch]$SkipBuild)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $root
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$final=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory}else{Join-Path $root $OutputDirectory}))
$packageRoot=[IO.Path]::GetFullPath((Join-Path $root 'out/packages')).TrimEnd('\')+'\'
if(-not $final.StartsWith($packageRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Candidate must stay inside out/packages.'}
if(Test-Path -LiteralPath $final){throw 'Use a new candidate name; existing evidence is preserved.'}
if(-not $SkipBuild){& (Join-Path $PSScriptRoot 'Build-Test.ps1') -Configuration Release;if($LASTEXITCODE){exit $LASTEXITCODE}}
$stage=Join-Path $packageRoot ('.portfolio-stage-'+[Guid]::NewGuid().ToString('N'))
& powershell -NoProfile -File (Join-Path $PSScriptRoot 'Build-GraphicsDemo.ps1') -OutputDirectory $stage -SkipBuild
if($LASTEXITCODE){throw 'G2 package foundation failed.'}
$binary=Join-Path $root 'out/build/windows-x64-release'
foreach($name in @('BenchmarkRunner','ResourceLifecycleBenchmark','ResourceLoading','EditorSandbox')){Copy-Item -LiteralPath (Join-Path $binary ($name+'.exe')) -Destination $stage}
foreach($directory in @('Textures','Models','Benchmark')){Copy-Item -LiteralPath (Join-Path $binary ('Resources/'+$directory)) -Destination (Join-Path $stage 'Resources') -Recurse}
New-Item -ItemType Directory -Path (Join-Path $stage 'Scripts') | Out-Null
foreach($name in @('Run-Portfolio.ps1','NativeProgram.ps1')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $stage 'Scripts')}
foreach($demo in @('materials','shadows','postprocess','repeated','mixed','mostly_outside','resources','loading-sync','loading-async','editor')){
    $launcher='@echo off'+"`r`n"+'"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Run-Portfolio.ps1" -Demo '+$demo+' %*'+"`r`n"+'exit /b %errorlevel%'+"`r`n"
    [IO.File]::WriteAllText((Join-Path $stage ($demo+'.cmd')),$launcher,[Text.Encoding]::ASCII)
}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=(& $vswhere -version '[17.0,18.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
$crt=$null
foreach($version in (Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Redist/MSVC') -Directory|Where-Object Name -match '^\d+\.\d+\.\d+$'|Sort-Object {[version]$_.Name} -Descending)){
    $candidate=Join-Path $version.FullName 'x64/Microsoft.VC143.CRT';if(Test-Path -LiteralPath $candidate){$crt=$candidate;break}
}
if(-not $crt){throw 'Installed MSVC x64 redistributable originals not found.'}
$crtFiles=@(Get-ChildItem -LiteralPath $crt -Filter '*.dll'|ForEach-Object{Copy-Item -LiteralPath $_.FullName -Destination $stage;[ordered]@{path=$_.Name;version=$_.VersionInfo.FileVersion;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
Write-EngineJson (Join-Path $stage 'runtime-dependencies.json') ([ordered]@{schema=1;architecture='x64';crt_version=$version.Name;crt=$crtFiles;system='Windows 10/11 x64, D3D12 FL11_0/SM5_1, DXGI, WIC, DirectWrite and Windows system API; installed GPU driver';sdk_required_to_run=$false;dotnet_required_to_run=$false;python_required_to_run=$false;system_font_redistributed=$false})
if((Invoke-EngineProgram (Join-Path $stage 'ResourceLoading.exe') @('--prepare-workload',(Join-Path $stage 'Resources/Loading')) (Join-Path $stage 'loading-workload.log')) -ne 0){throw 'Fixed loading workload generation failed.'}
Copy-Item -LiteralPath (Join-Path $root 'Samples/Portfolio/Package-README.txt') -Destination (Join-Path $stage 'README.txt') -Force
$source=Get-EngineSourceIdentity $root
Write-EngineJson (Join-Path $stage 'source-inputs.json') $source
& python (Join-Path $PSScriptRoot 'PortfolioEvidence.py') --root $root --package $stage --evidence ([IO.Path]::GetFullPath((Join-Path $root $EvidenceDirectory)))
if($LASTEXITCODE){throw 'Evidence/source closure failed.'}
if((Get-EngineSourceIdentity $root).source_sha256 -ne $source.source_sha256){throw 'Source changed during packaging.'}
$files=@(Get-ChildItem -LiteralPath $stage -File -Recurse|Where-Object {$_.FullName -ne (Join-Path $stage 'manifest.json')}|Sort-Object FullName|ForEach-Object{[ordered]@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
if(@($files|Where-Object {$_.path -match '(^|/)AGENTS\.md$'}).Count){throw 'Local AGENTS must not enter a delivery.'}
$totalBytes=0L;foreach($file in $files){$totalBytes+=[long]$file.bytes}
$manifest=[ordered]@{schema=1;kind='PortfolioCandidate';configuration='Release';build_id=[IO.Path]::GetFileName($final);git_head=$source.git_head;dirty=$source.dirty;source_sha256=$source.source_sha256;
    current_source='SourceArchive';historical_sources='evidence-index.json';runtime='Windows x64 / app-local MSVC / D3D12';source_hdr_required=$false;clean_machine_tested=$false;complete_v1=$false;files=$files;file_count=$files.Count;total_bytes=$totalBytes}
Write-EngineJson (Join-Path $stage 'manifest.json') $manifest
# 移动前再次核对最终绝对路径和工作区边界；失败 staging 保留，便于诊断。
if(-not ([IO.Path]::GetFullPath($stage)).StartsWith($packageRoot,[StringComparison]::OrdinalIgnoreCase) -or -not $final.StartsWith($packageRoot,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $final)){throw 'Candidate move boundary changed.'}
Move-Item -LiteralPath $stage -Destination $final
Write-Output ('PortfolioCandidate: '+$final+' / '+$source.source_sha256+' / '+$files.Count+' files')
exit 0
