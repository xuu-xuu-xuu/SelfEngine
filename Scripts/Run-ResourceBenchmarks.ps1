param([string]$OutputDirectory='out/benchmarks/resources-01',[int]$Repeats=3,[switch]$Smoke)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $projectRoot
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$outputPath=[IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
if(-not $outputPath.StartsWith($projectRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Resource data must remain inside workspace.'}
if(Test-Path -LiteralPath $outputPath){throw 'Use a new resource benchmark directory.'}
if($Repeats -lt 1 -or $Repeats -gt 10 -or (-not $Smoke -and $Repeats -lt 3)){throw 'Formal resource protocol needs at least three repeats.'}
$binary=Join-Path $projectRoot 'out/build/windows-x64-release/ResourceLifecycleBenchmark.exe'
New-Item -ItemType Directory -Path $outputPath | Out-Null
$source=Get-EngineSourceIdentity $projectRoot
Write-EngineJson (Join-Path $outputPath 'source-inputs.json') $source
$os=Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion'
$cpu=Get-ItemProperty -LiteralPath 'HKLM:/HARDWARE/DESCRIPTION/System/CentralProcessor/0'
$powerLine='unavailable'
try{Add-Type -AssemblyName System.Windows.Forms;$powerLine=[System.Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus.ToString()}catch{}
$environment=[ordered]@{schema=1;suite='Resources';source_sha256=$source.source_sha256;git_head=$source.git_head;dirty=$source.dirty;smoke=[bool]$Smoke;repetitions=$Repeats;
    executable_sha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant();os_registry_product=$os.ProductName;os_display_version=$os.DisplayVersion;os_build=$os.CurrentBuildNumber;os_ubr=$os.UBR;
    cpu_brand=$cpu.ProcessorNameString;hardware_threads=[Environment]::ProcessorCount;power_line_status=$powerLine;power_scheme=(& powercfg /getactivescheme|Out-String).Trim();background_load='ordinary desktop, not isolated';power_settings_changed=$false}
Write-EngineJson (Join-Path $outputPath 'environment.json') $environment
function Read-ResourceSensor([string]$LogPath){
    $sensor=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
    if(-not $sensor){return [ordered]@{available=$false;reason='nvidia-smi unavailable'}}
    try{$code=Invoke-EngineProgram $sensor.Source @('--query-gpu=name,driver_version,temperature.gpu,power.draw,clocks.current.graphics','--format=csv,noheader,nounits') $LogPath
        return [ordered]@{available=($code -eq 0);utc=[DateTime]::UtcNow.ToString('o');raw_csv=[IO.File]::ReadAllText($LogPath).Trim();fields='name,driver_version,temperature.gpu C,power.draw W,graphics MHz'}
    }catch{return [ordered]@{available=$false;reason=$_.Exception.Message}}
}
$cases=@([PSCustomObject]@{name='baseline';memory='placed';upload='batch';cache='on'},[PSCustomObject]@{name='committed';memory='committed';upload='batch';cache='on'},
    [PSCustomObject]@{name='individual';memory='placed';upload='individual';cache='on'},[PSCustomObject]@{name='uncached';memory='placed';upload='batch';cache='off'})
$captures=Join-Path $outputPath 'captures';New-Item -ItemType Directory -Path $captures | Out-Null
foreach($case in $cases){
    $arguments=@('--output',(Join-Path $captures $case.name),'--source-id',$source.source_sha256,'--memory',$case.memory,'--upload',$case.upload,'--pso-cache',$case.cache,
        '--capture',(Join-Path $captures ($case.name+'.ppm')),'--cycles','1','--warmup-cycles','0')
    if((Invoke-EngineProgram $binary $arguments (Join-Path $captures ($case.name+'.log'))) -ne 0){throw ('Resource capture failed: '+$case.name)}
}
for($round=1;$round -le $Repeats;$round++){
    $order=if($round%2){$cases}else{@($cases[3],$cases[2],$cases[1],$cases[0])}
    foreach($case in $order){
        $name=$case.name+'-'+$round;$directory=Join-Path $outputPath $name
        $arguments=@('--output',$directory,'--source-id',$source.source_sha256,'--memory',$case.memory,'--upload',$case.upload,'--pso-cache',$case.cache)
        if($Smoke){$arguments+=@('--light','--cycles','4','--warmup-cycles','2')}
        $before=Read-ResourceSensor (Join-Path $outputPath ($name+'-sensor-before.log'))
        Write-Output ('Starting resource '+$name+$(if($Smoke){' (short smoke)'}else{' (4 warmup + 24 measured cycles)'}))
        if((Invoke-EngineProgram $binary $arguments (Join-Path $outputPath ($name+'.log'))) -ne 0){throw ('Resource cycle failed: '+$name)}
        $after=Read-ResourceSensor (Join-Path $outputPath ($name+'-sensor-after.log'))
        Write-EngineJson (Join-Path $directory 'sensors.json') ([ordered]@{before=$before;after=$after})
        Write-Output ('Completed resource '+$name)
    }
}
$afterSource=Get-EngineSourceIdentity $projectRoot
if($afterSource.source_sha256 -ne $source.source_sha256){throw 'Resource benchmark source changed during collection.'}
$analysis=@((Join-Path $PSScriptRoot 'Analyze-Resources.py'),'--input',$outputPath)
if(-not $Smoke){$analysis+='--formal'}
& python @analysis
if($LASTEXITCODE -ne 0){throw 'Resource data validation failed.'}
Write-Output ('Resource benchmark protocol completed: '+$outputPath)
exit 0
