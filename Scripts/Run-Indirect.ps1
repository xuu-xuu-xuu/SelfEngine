param([string]$OutputDirectory='out/benchmarks/indirect',[ValidateSet('Debug','Release')][string]$Configuration='Release',[switch]$Smoke)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $root
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
if(!$Smoke -and $Configuration -ne 'Release'){throw 'Formal indirect measurements require Release.'}
$output=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory}else{Join-Path $root $OutputDirectory}))
if(!$output.StartsWith($root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Evidence must stay inside the workspace.'}
if(Test-Path -LiteralPath $output){throw 'Use a fresh output directory.'}
New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
$build=Join-Path $root ('out/build/windows-x64-'+$Configuration.ToLowerInvariant())
$binary=Join-Path $build 'IndirectBenchmark.exe'
function Read-IndirectRuntime {
    $result=[ordered]@{}
    foreach($directory in @('Resources/Shaders','Resources/Localization','Experiments')){
        foreach($file in (Get-ChildItem -LiteralPath (Join-Path $build $directory) -File -Recurse | Sort-Object FullName)){
            $relative=$file.FullName.Substring($build.Length+1).Replace('\','/')
            $result[$relative]=(Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()
        }
    }
    return $result
}
$source=Get-EngineSourceIdentity $root
$binaryHash=(Get-FileHash -LiteralPath $binary).Hash.ToLowerInvariant()
$runtime=Read-IndirectRuntime
$copies=@{}
foreach($name in @('Indirect.hlsl','Visibility.hlsl','VisibilityPredicate.hlsli')){$copies['Experiments/'+$name]='Resources/Shaders/'+$name}
foreach($name in @('PbrInstanced.hlsl','Pbr.hlsl','PbrBindings.hlsli','InstancingBindings.hlsli')){$copies['Resources/Shaders/'+$name]='Samples/Materials/'+$name}
foreach($name in @('en-US.txt','zh-CN.txt')){$copies['Resources/Localization/'+$name]='Resources/Localization/'+$name}
foreach($key in $copies.Keys){if($runtime[$key] -ne (Get-FileHash -LiteralPath (Join-Path $root $copies[$key])).Hash.ToLowerInvariant()){throw ('Copied runtime input is stale: '+$key)}}
function Read-IndirectSensor {
    $command=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
    if(!$command){return @{available=$false}}
    $sensor=(& $command.Source --query-gpu=name,driver_version,temperature.gpu,power.draw,clocks.current.graphics --format=csv,noheader,nounits 2>&1|Out-String).Trim()
    return @{available=($LASTEXITCODE -eq 0);utc=[DateTime]::UtcNow.ToString('o');fields='name,driver_version,temperature C,power W,graphics MHz';raw_csv=$sensor}
}
$os=Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion'
$cpu=Get-ItemProperty -LiteralPath 'HKLM:/HARDWARE/DESCRIPTION/System/CentralProcessor/0'
$environment=[ordered]@{schema=2;source_sha256=$source.source_sha256;git_head=$source.git_head;dirty=$source.dirty;executable_sha256=$binaryHash;runtime_sha256=$runtime;
    configuration=$Configuration;smoke=[bool]$Smoke;os_build=$os.CurrentBuildNumber;os_ubr=$os.UBR;power_scheme=(& powercfg /getactivescheme|Out-String).Trim();
    cpu_name=$cpu.ProcessorNameString.Trim();logical_processors=[int]$env:NUMBER_OF_PROCESSORS;
    power_settings_changed=$false;background_load='ordinary desktop, not isolated';continuous_clock_monitoring=$false;before=(Read-IndirectSensor)}
Write-EngineJson ($output+'.environment-before.json') $environment
$arguments=@('--benchmark','--language','en-US','--output',$output,'--source-id',$source.source_sha256)
if($Smoke){$arguments+='--smoke'}
if((Invoke-EngineProgram $binary $arguments ($output+'.log')) -ne 0){throw 'Indirect benchmark failed; raw output retained.'}
$environment.after=Read-IndirectSensor
$after=Read-IndirectRuntime
$runtimeUnchanged=($after.Count -eq $runtime.Count)
foreach($key in $runtime.Keys){if($runtime[$key] -ne $after[$key]){$runtimeUnchanged=$false}}
$environment.source_and_binaries_unchanged=((Get-EngineSourceIdentity $root).source_sha256 -eq $source.source_sha256 -and (Get-FileHash -LiteralPath $binary).Hash.ToLowerInvariant() -eq $binaryHash -and $runtimeUnchanged)
Write-EngineJson (Join-Path $output 'environment.json') $environment
Write-EngineJson (Join-Path $output 'source-inputs.json') $source
if(!$environment.source_and_binaries_unchanged){throw 'Measurement inputs changed.'}
$analysis=@((Join-Path $PSScriptRoot 'Analyze-Indirect.py'),'--input',$output)
if(!$Smoke){$analysis+='--formal'}
& python @analysis
if($LASTEXITCODE -ne 0){throw 'Indirect analysis failed.'}
Write-Output ('Indirect experiment completed: '+$output)
