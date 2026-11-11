param([string]$OutputDirectory='out/benchmarks/system',[int]$Repeats=3,[switch]$Smoke,[ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $projectRoot
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$outputPath=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory}else{Join-Path $projectRoot $OutputDirectory}))
if(-not $outputPath.StartsWith($projectRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Benchmark data must stay inside the workspace.'}
if(Test-Path -LiteralPath $outputPath){throw 'Use a new benchmark output directory.'}
if($Repeats -lt 1 -or $Repeats -gt 10 -or (-not $Smoke -and ($Repeats -lt 3 -or $Configuration -ne 'Release'))){throw 'Formal protocol requires Release and at least three repetitions.'}
$directory=Join-Path $projectRoot ('out/managed/SystemBenchmark/'+$Configuration+'/net10.0')
$binary=Join-Path $directory 'SelfEngine.SystemBenchmark.dll'
$dotnet=(Get-Command dotnet.exe -ErrorAction Stop).Source
$source=Get-EngineSourceIdentity $projectRoot
New-Item -ItemType Directory -Path $outputPath | Out-Null
Write-EngineJson (Join-Path $outputPath 'source-inputs.json') $source
$binaryHashes=@(foreach($name in @('SelfEngine.SystemBenchmark.dll','SelfEngine.Managed.dll','EngineNative.dll')){[ordered]@{name=$name;sha256=(Get-FileHash -LiteralPath (Join-Path $directory $name)).Hash.ToLowerInvariant()}})
$os=Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion'
Write-EngineJson (Join-Path $outputPath 'environment.json') ([ordered]@{schema=1;suite='System';source_sha256=$source.source_sha256;git_head=$source.git_head;dirty=$source.dirty;smoke=[bool]$Smoke;repetitions=$Repeats;configuration=$Configuration;binary_hashes=$binaryHashes;os_build=$os.CurrentBuildNumber;os_ubr=$os.UBR;power_scheme=(& powercfg /getactivescheme|Out-String).Trim();power_settings_changed=$false;background_load='ordinary desktop, not isolated'})
$sensor=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
function Read-Sensor{
    if(-not $sensor){return @{available=$false}}
    $raw=(& $sensor.Source --query-gpu=name,driver_version,temperature.gpu,power.draw,clocks.current.graphics --format=csv,noheader,nounits 2>&1|Out-String).Trim()
    return @{available=($LASTEXITCODE -eq 0);utc=[DateTime]::UtcNow.ToString('o');raw_csv=$raw;fields='name,driver_version,temperature.gpu C,power.draw W,graphics MHz'}
}
$variants=@(0..7|ForEach-Object{[pscustomobject]@{name='s'+($_ -band 1)+'p'+(($_ -shr 1) -band 1)+'u'+(($_ -shr 2) -band 1);scripts=$(if($_ -band 1){'on'}else{'off'});physics=$(if($_ -band 2){'on'}else{'off'});ui=$(if($_ -band 4){'on'}else{'off'})}})
function Run-Variant($variant,[string]$target,[string]$capture=''){
    $arguments=@($binary,'--output',$target,'--source-id',$source.source_sha256,'--scripts',$variant.scripts,'--physics',$variant.physics,'--ui',$variant.ui)
    if($Smoke){$arguments+=@('--warmup','0.5','--seconds','1')}
    if($capture){$arguments+=@('--capture',$capture)}
    $before=Read-Sensor
    if((Invoke-EngineProgram $dotnet $arguments ($target+'.log') -VisibleWindow) -ne 0){throw ('System benchmark failed: '+$target)}
    Write-EngineJson (Join-Path $target 'sensors.json') @{before=$before;after=(Read-Sensor)}
}
$captures=Join-Path $outputPath 'captures';New-Item -ItemType Directory -Path $captures | Out-Null
foreach($variant in $variants){Run-Variant $variant (Join-Path $captures $variant.name) (Join-Path $captures ($variant.name+'.ppm'))}
for($round=1;$round -le $Repeats;$round++){
    $order=if($round%2){$variants}else{@($variants[7..0])}
    foreach($variant in $order){$name=$variant.name+'-'+$round;Write-Output ('Starting '+$name);Run-Variant $variant (Join-Path $outputPath $name);Write-Output ('Completed '+$name)}
}
if((Get-EngineSourceIdentity $projectRoot).source_sha256 -ne $source.source_sha256){throw 'Source inputs changed during acquisition.'}
foreach($item in $binaryHashes){if((Get-FileHash -LiteralPath (Join-Path $directory $item.name)).Hash.ToLowerInvariant() -ne $item.sha256){throw 'Binary changed during acquisition.'}}
$analysis=@((Join-Path $PSScriptRoot 'Analyze-SystemBenchmarks.py'),'--input',$outputPath)
if(-not $Smoke){$analysis+='--formal'}
& python @analysis
if($LASTEXITCODE -ne 0){throw 'System benchmark validation failed.'}
Write-Output ('System protocol completed: '+$outputPath)
