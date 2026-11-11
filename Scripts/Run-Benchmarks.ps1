param([string]$OutputDirectory='out/benchmarks/baseline',[int]$Repeats=3,[switch]$Smoke,[ValidateSet('Baseline','Culling','Sorting','Instancing','Presentation')][string]$Suite='Baseline')
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $projectRoot
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$outputPath=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory}else{Join-Path $projectRoot $OutputDirectory}))
if(-not $outputPath.StartsWith($projectRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Benchmark data must stay inside the workspace.'}
if(Test-Path -LiteralPath $outputPath){throw 'Use a new benchmark output directory.'}
if($Repeats -lt 1 -or $Repeats -gt 10){throw 'Repeats must be in [1,10].'}
if(-not $Smoke -and $Repeats -lt 3){throw 'Formal protocol needs at least three repetitions.'}
$binary=Join-Path $projectRoot 'out/build/windows-x64-release/BenchmarkRunner.exe'
if(-not (Test-Path -LiteralPath $binary)){throw 'Build and test the Release BenchmarkRunner first.'}
New-Item -ItemType Directory -Path $outputPath | Out-Null
$source=Get-EngineSourceIdentity $projectRoot
Write-EngineJson (Join-Path $outputPath 'source-inputs.json') $source
$os=Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion'
$environment=[ordered]@{schema=1;suite=$Suite;source_sha256=$source.source_sha256;git_head=$source.git_head;dirty=$source.dirty;smoke=[bool]$Smoke;repetitions=$Repeats;
    executable_sha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant();os_registry_product=$os.ProductName;os_display_version=$os.DisplayVersion;os_build=$os.CurrentBuildNumber;os_ubr=$os.UBR;
    power_scheme=(& powercfg /getactivescheme|Out-String).Trim();background_load='ordinary desktop, not isolated';power_settings_changed=$false}
Write-EngineJson (Join-Path $outputPath 'environment.json') $environment
$sensor=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
function Read-GpuSensor{
    if(-not $sensor){return [ordered]@{available=$false;reason='nvidia-smi unavailable'}}
    $text=(& $sensor.Source --query-gpu=name,driver_version,temperature.gpu,power.draw,clocks.current.graphics --format=csv,noheader,nounits 2>&1|Out-String).Trim()
    return [ordered]@{available=($LASTEXITCODE -eq 0);utc=[DateTime]::UtcNow.ToString('o');raw_csv=$text;fields='name,driver_version,temperature.gpu C,power.draw W,graphics MHz'}
}
$jobs=@()
if($Suite -eq 'Baseline'){
    foreach($name in @('repeated','mixed')){for($round=1;$round -le $Repeats;$round++){
        $jobs+=[PSCustomObject]@{name=$name+'-'+$round;config=$name;culling='off'}
    }}
}elseif($Suite -eq 'Culling'){
    $captures=Join-Path $outputPath 'captures'
    New-Item -ItemType Directory -Path $captures | Out-Null
    foreach($name in @('all_visible','mostly_outside')){foreach($switchValue in @('off','on')){
        $captureName=$name+'-'+$switchValue
        $arguments=@('--config',(Join-Path $projectRoot ('Resources/Samples/Benchmark/'+$name+'.json')),'--output',(Join-Path $captures $captureName),
            '--source-id',$source.source_sha256,'--culling',$switchValue,'--capture',(Join-Path $captures ($captureName+'.ppm')))
        if((Invoke-EngineProgram $binary $arguments (Join-Path $captures ($captureName+'.log'))) -ne 0){throw ('Culling capture failed: '+$captureName)}
    }}
    for($round=1;$round -le $Repeats;$round++){
        $switches=if($round%2){@('off','on')}else{@('on','off')}
        foreach($name in @('all_visible','mostly_outside')){foreach($switchValue in $switches){
            $jobs+=[PSCustomObject]@{name=$name+'-'+$switchValue+'-'+$round;config=$name;culling=$switchValue}
        }}
    }
}elseif($Suite -eq 'Sorting'){
    $variants=@([PSCustomObject]@{name='raw';sorting='off';bindings='off'},[PSCustomObject]@{name='cache';sorting='off';bindings='on'},[PSCustomObject]@{name='sorted';sorting='on';bindings='on'})
    $captures=Join-Path $outputPath 'captures'
    New-Item -ItemType Directory -Path $captures | Out-Null
    foreach($variant in $variants){
        $arguments=@('--config',(Join-Path $projectRoot 'Resources/Samples/Benchmark/all_visible.json'),'--output',(Join-Path $captures $variant.name),
            '--source-id',$source.source_sha256,'--culling','off','--sorting',$variant.sorting,'--binding-cache',$variant.bindings,'--capture',(Join-Path $captures ($variant.name+'.ppm')))
        if((Invoke-EngineProgram $binary $arguments (Join-Path $captures ($variant.name+'.log'))) -ne 0){throw ('Sorting capture failed: '+$variant.name)}
    }
    for($round=1;$round -le $Repeats;$round++){
        $order=if($round%2){$variants}else{@($variants[2],$variants[1],$variants[0])}
        foreach($variant in $order){$jobs+=[PSCustomObject]@{name=$variant.name+'-'+$round;config='all_visible';culling='off';sorting=$variant.sorting;bindings=$variant.bindings}}
    }
}elseif($Suite -eq 'Presentation'){
    $captures=Join-Path $outputPath 'captures';New-Item -ItemType Directory -Path $captures | Out-Null
    foreach($view in @('visible','hidden')){
        $arguments=@('--config',(Join-Path $projectRoot 'Resources/Samples/Benchmark/mixed.json'),'--output',(Join-Path $captures $view),
            '--source-id',$source.source_sha256,'--count','32','--culling','off','--sorting','off','--binding-cache','off','--instancing','off','--capture',(Join-Path $captures ($view+'.ppm')))
        if($view -eq 'visible'){$arguments+='--show'}
        if((Invoke-EngineProgram $binary $arguments (Join-Path $captures ($view+'.log')) -VisibleWindow:($view -eq 'visible')) -ne 0){throw ('Presentation capture failed: '+$view)}
    }
    for($round=1;$round -le $Repeats;$round++){
        $order=if($round%2){@('visible','hidden')}else{@('hidden','visible')}
        foreach($view in $order){$jobs+=[PSCustomObject]@{name=$view+'-'+$round;config='mixed';culling='off';show=($view -eq 'visible')}}
    }
}else{
    $captures=Join-Path $outputPath 'captures';New-Item -ItemType Directory -Path $captures | Out-Null
    foreach($name in @('repeated','mixed','mostly_outside')){foreach($enabled in @('off','on')){
        $captureName=$name+'-'+$enabled;$culling=if($name -eq 'mostly_outside'){'on'}else{'off'}
        $arguments=@('--config',(Join-Path $projectRoot ('Resources/Samples/Benchmark/'+$name+'.json')),'--output',(Join-Path $captures $captureName),
            '--source-id',$source.source_sha256,'--sorting','off','--binding-cache','on','--culling',$culling,'--instancing',$enabled,'--capture',(Join-Path $captures ($captureName+'.ppm')))
        if((Invoke-EngineProgram $binary $arguments (Join-Path $captures ($captureName+'.log'))) -ne 0){throw ('Instance capture failed: '+$captureName)}
    }}
    for($round=1;$round -le $Repeats;$round++){
        $switches=if($round%2){@('off','on')}else{@('on','off')}
        foreach($name in @('repeated','mixed')){foreach($enabled in $switches){$jobs+=[PSCustomObject]@{name=$name+'-'+$enabled+'-'+$round;config=$name;culling='off';instancing=$enabled}}}
    }
}
foreach($job in $jobs){
        $runName=$job.name
        $directory=Join-Path $outputPath $runName
        $before=Read-GpuSensor
        $arguments=@('--config',(Join-Path $projectRoot ('Resources/Samples/Benchmark/'+$job.config+'.json')),'--output',$directory,'--source-id',$source.source_sha256,'--culling',$job.culling)
        $sortingValue=if($Suite -eq 'Sorting'){$job.sorting}else{'off'}
        $bindingValue=if($Suite -eq 'Sorting'){$job.bindings}else{'off'}
        if($Suite -eq 'Instancing'){$bindingValue='on'}
        $arguments+=@('--sorting',$sortingValue,'--binding-cache',$bindingValue)
        $instanceValue=if($Suite -eq 'Instancing'){$job.instancing}else{'off'}
        $arguments+=@('--instancing',$instanceValue)
        if($Suite -eq 'Presentation'){if(-not $Smoke){$arguments+=@('--count','32')};if($job.show){$arguments+='--show'}}
        if($Smoke){$warmupValue=if($Suite -eq 'Presentation'){'0.5'}else{'0.05'};$sampleValue=if($Suite -eq 'Presentation'){'1'}else{'0.2'};$arguments+=@('--count','32','--warmup',$warmupValue,'--seconds',$sampleValue)}
        Write-Output ('Starting '+$runName+$(if($Smoke){' (smoke)'}else{' (5s warmup + 60s sample)'}))
        if((Invoke-EngineProgram $binary $arguments (Join-Path $outputPath ($runName+'.log')) -VisibleWindow:($Suite -eq 'Presentation' -and $job.show)) -ne 0){throw ('Benchmark failed: '+$runName)}
        $after=Read-GpuSensor
        Write-EngineJson (Join-Path $directory 'sensors.json') ([ordered]@{before=$before;after=$after})
        Write-Output ('Completed '+$runName)
}
$afterSource=Get-EngineSourceIdentity $projectRoot
if($afterSource.source_sha256 -ne $source.source_sha256){throw 'Source inputs changed during acquisition; retain data but do not label this as one frozen version.'}
$analyzer=if($Suite -eq 'Presentation'){'Analyze-Presentation.py'}elseif($Suite -eq 'Culling'){'Analyze-Culling.py'}elseif($Suite -eq 'Sorting'){'Analyze-Sorting.py'}elseif($Suite -eq 'Instancing'){'Analyze-Instancing.py'}else{'Analyze-Benchmarks.py'}
$analysis=@((Join-Path $PSScriptRoot $analyzer),'--input',$outputPath)
if(-not $Smoke){$analysis+='--formal'}
& python @analysis
if($LASTEXITCODE -ne 0){throw 'Benchmark data validation/analysis failed.'}
Write-Output ('Benchmark protocol completed: '+$outputPath)
exit 0
