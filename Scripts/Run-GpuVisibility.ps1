param([string]$OutputDirectory='out/benchmarks/gpu-visibility',[ValidateSet('Debug','Release')][string]$Configuration='Release',[switch]$Smoke)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $root
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
if(!$Smoke -and $Configuration -ne 'Release'){throw 'Formal visibility measurements require Release.'}
$output=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory}else{Join-Path $root $OutputDirectory}))
if(!$output.StartsWith($root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Evidence must stay inside the workspace.'}
if(Test-Path -LiteralPath $output){throw 'Use a fresh output directory.'}
New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
$build=Join-Path $root ('out/build/windows-x64-'+$Configuration.ToLowerInvariant())
$binary=Join-Path $build 'VisibilityBenchmark.exe'
$shader=Join-Path $build 'Experiments/Visibility.hlsl'
$predicate=Join-Path $build 'Experiments/VisibilityPredicate.hlsli'
$source=Get-EngineSourceIdentity $root
$binaryHash=(Get-FileHash -LiteralPath $binary).Hash.ToLowerInvariant()
$shaderHash=(Get-FileHash -LiteralPath $shader).Hash.ToLowerInvariant()
$predicateHash=(Get-FileHash -LiteralPath $predicate).Hash.ToLowerInvariant()
if($shaderHash -ne (Get-FileHash -LiteralPath (Join-Path $root 'Resources/Shaders/Visibility.hlsl')).Hash.ToLowerInvariant()){throw 'Copied shader is stale.'}
if($predicateHash -ne (Get-FileHash -LiteralPath (Join-Path $root 'Resources/Shaders/VisibilityPredicate.hlsli')).Hash.ToLowerInvariant()){throw 'Copied shader predicate is stale.'}
function Read-VisibilitySensor{
    $command=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
    if(!$command){return @{available=$false}}
    $sensor=(& $command.Source --query-gpu=name,driver_version,temperature.gpu,power.draw,clocks.current.graphics --format=csv,noheader,nounits 2>&1|Out-String).Trim()
    return @{available=($LASTEXITCODE -eq 0);utc=[DateTime]::UtcNow.ToString('o');fields='name,driver_version,temperature C,power W,graphics MHz';raw_csv=$sensor}
}
$os=Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion'
$cpu=Get-ItemProperty -LiteralPath 'HKLM:/HARDWARE/DESCRIPTION/System/CentralProcessor/0'
$environment=[ordered]@{schema=2;source_sha256=$source.source_sha256;git_head=$source.git_head;dirty=$source.dirty;executable_sha256=$binaryHash;shader_sha256=$shaderHash;predicate_sha256=$predicateHash;
    configuration=$Configuration;smoke=[bool]$Smoke;os_build=$os.CurrentBuildNumber;os_ubr=$os.UBR;power_scheme=(& powercfg /getactivescheme|Out-String).Trim();
    cpu_name=$cpu.ProcessorNameString.Trim();logical_processors=[int]$env:NUMBER_OF_PROCESSORS;
    power_settings_changed=$false;background_load='ordinary desktop, not isolated';before=(Read-VisibilitySensor)}
Write-EngineJson ($output+'.environment-before.json') $environment
$arguments=@('--output',$output,'--source-id',$source.source_sha256)
if($Smoke){$arguments+='--smoke'}
if((Invoke-EngineProgram $binary $arguments ($output+'.log')) -ne 0){throw 'Visibility benchmark failed; raw output retained.'}
$environment.after=Read-VisibilitySensor
$environment.source_and_binaries_unchanged=((Get-EngineSourceIdentity $root).source_sha256 -eq $source.source_sha256 -and (Get-FileHash -LiteralPath $binary).Hash.ToLowerInvariant() -eq $binaryHash -and (Get-FileHash -LiteralPath $shader).Hash.ToLowerInvariant() -eq $shaderHash -and (Get-FileHash -LiteralPath $predicate).Hash.ToLowerInvariant() -eq $predicateHash)
Write-EngineJson (Join-Path $output 'environment.json') $environment
Write-EngineJson (Join-Path $output 'source-inputs.json') $source
if(!$environment.source_and_binaries_unchanged){throw 'Measurement inputs changed.'}
$analysis=@((Join-Path $PSScriptRoot 'Analyze-GpuVisibility.py'),'--input',$output)
if(!$Smoke){$analysis+='--formal'}
& python @analysis
if($LASTEXITCODE -ne 0){throw 'Visibility analysis failed.'}
Write-Output ('GPU visibility experiment completed: '+$output)
