param([string]$OutputDirectory='out/benchmarks/loading-01',[int]$Repeats=3,[switch]$Smoke)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $projectRoot
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
$outputPath=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory}else{Join-Path $projectRoot $OutputDirectory}))
if(-not $outputPath.StartsWith($projectRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Loading data must remain in workspace.'}
if(Test-Path -LiteralPath $outputPath){throw 'Use a new loading benchmark directory.'}
if($Repeats -lt 1 -or $Repeats -gt 10 -or (-not $Smoke -and $Repeats -lt 3)){throw 'Formal loading protocol requires three repeats.'}
$binary=Join-Path $projectRoot 'out/build/windows-x64-release/ResourceLoading.exe'
New-Item -ItemType Directory -Path $outputPath | Out-Null
$source=Get-EngineSourceIdentity $projectRoot;Write-EngineJson (Join-Path $outputPath 'source-inputs.json') $source
$os=Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion'
$cpu=Get-ItemProperty -LiteralPath 'HKLM:/HARDWARE/DESCRIPTION/System/CentralProcessor/0'
$environment=[ordered]@{schema=1;suite='ResourceLoading';source_sha256=$source.source_sha256;git_head=$source.git_head;dirty=$source.dirty;smoke=[bool]$Smoke;repetitions=$Repeats;
    executable_sha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant();os_registry_product=$os.ProductName;os_display_version=$os.DisplayVersion;os_build=$os.CurrentBuildNumber;os_ubr=$os.UBR;
    cpu_brand=$cpu.ProcessorNameString;hardware_threads=[Environment]::ProcessorCount;power_scheme=(& powercfg /getactivescheme|Out-String).Trim();background_load='ordinary desktop, not isolated';power_settings_changed=$false}
Write-EngineJson (Join-Path $outputPath 'environment.json') $environment
function Read-LoadingSensor([string]$LogPath){
    $sensor=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
    if(-not $sensor){return [ordered]@{available=$false;reason='nvidia-smi unavailable'}}
    try{$code=Invoke-EngineProgram $sensor.Source @('--query-gpu=name,driver_version,temperature.gpu,power.draw,clocks.current.graphics','--format=csv,noheader,nounits') $LogPath
        return [ordered]@{available=($code -eq 0);utc=[DateTime]::UtcNow.ToString('o');raw_csv=[IO.File]::ReadAllText($LogPath).Trim();fields='name,driver_version,temperature.gpu C,power.draw W,graphics MHz'}
    }catch{return [ordered]@{available=$false;reason=$_.Exception.Message}}
}
$workload=Join-Path $outputPath 'workload';$prepare=@('--prepare-workload',$workload);if($Smoke){$prepare+='--light'}
if((Invoke-EngineProgram $binary $prepare (Join-Path $outputPath 'prepare.log')) -ne 0){throw 'Loading fixture creation failed.'}
$common=@('--workload',$workload,'--source-id',$source.source_sha256)
if($Smoke){$common+=@('--frames','96','--trigger-frame','8','--frame-ms','0','--budget-bytes','4096','--budget-tasks','2')}
$captures=Join-Path $outputPath 'captures';New-Item -ItemType Directory -Path $captures | Out-Null
foreach($mode in @('sync','async')){
    $arguments=$common+@('--mode',$mode,'--output',(Join-Path $captures $mode),'--capture',(Join-Path $captures ($mode+'.ppm')))
    if((Invoke-EngineProgram $binary $arguments (Join-Path $captures ($mode+'.log'))) -ne 0){throw ('Loading final capture failed: '+$mode)}
}
for($round=1;$round -le $Repeats;$round++){
    $order=if($round%2){@('sync','async')}else{@('async','sync')}
    foreach($mode in $order){
        $name=$mode+'-'+$round;$directory=Join-Path $outputPath $name;$arguments=$common+@('--mode',$mode,'--output',$directory)
        $before=Read-LoadingSensor (Join-Path $outputPath ($name+'-sensor-before.log'))
        Write-Output ('Starting loading '+$name+$(if($Smoke){' (short smoke)'}else{' (480 frames, 16 x 1025x513 textures)'}))
        if((Invoke-EngineProgram $binary $arguments (Join-Path $outputPath ($name+'.log'))) -ne 0){throw ('Loading run failed: '+$name)}
        $after=Read-LoadingSensor (Join-Path $outputPath ($name+'-sensor-after.log'))
        Write-EngineJson (Join-Path $directory 'sensors.json') ([ordered]@{before=$before;after=$after});Write-Output ('Completed loading '+$name)
    }
}
$afterSource=Get-EngineSourceIdentity $projectRoot
if($afterSource.source_sha256 -ne $source.source_sha256){throw 'Loading source changed during collection.'}
$arguments=@((Join-Path $PSScriptRoot 'Analyze-Loading.py'),'--input',$outputPath);if(-not $Smoke){$arguments+='--formal'}
& python @arguments
if($LASTEXITCODE -ne 0){throw 'Loading analysis failed.'}
Write-Output ('Loading protocol completed: '+$outputPath)
exit 0
