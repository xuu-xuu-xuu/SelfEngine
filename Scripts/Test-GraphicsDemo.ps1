param([Parameter(Mandatory=$true)][string]$PackageDirectory,[Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
function Assert-DemoLog([string]$Log){
    $bad=@(Get-Content -LiteralPath $Log -Encoding UTF8|Where-Object {$_ -match '\[Error\]|\[Warning\]' -and $_ -notmatch '^\[Warning\] Long frame limited; raw/update dt in milliseconds'})
    if($bad.Count){throw ('Player emitted diagnostics: '+$Log)}
}
$packagePath=(Resolve-Path -LiteralPath $PackageDirectory).Path
$outputPath=[IO.Path]::GetFullPath($OutputDirectory)
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(-not $outputPath.StartsWith($projectRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Demo evidence must stay inside the workspace.'}
if(Test-Path -LiteralPath $outputPath){throw 'Use a new test-output directory; existing evidence is preserved.'}
New-Item -ItemType Directory -Path $outputPath | Out-Null
$manifest=Get-Content -LiteralPath (Join-Path $packagePath 'manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
if($manifest.schema -ne 1 -or $manifest.configuration -ne 'Release'){throw 'Expected a Release package manifest.'}
foreach($file in $manifest.files){
    $path=[IO.Path]::GetFullPath((Join-Path $packagePath $file.path))
    if(-not $path.StartsWith($packagePath.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Manifest path escaped package.'}
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){throw ('Package digest mismatch: '+$file.path)}
}
if(Get-ChildItem -LiteralPath (Join-Path $packagePath 'Resources/Environments') -Filter '*.hdr'){throw 'Package must load baked environments without source HDR.'}
$foreign=Join-Path $outputPath 'foreign cwd'
New-Item -ItemType Directory -Path $foreign | Out-Null
$executable=Join-Path $packagePath 'EngineSandbox.exe'
$oldPath=$env:PATH
$results=[Collections.Generic.List[object]]::new()
try{
    $env:PATH=(Join-Path $env:SystemRoot 'System32')+';'+$env:SystemRoot
    Push-Location -LiteralPath $foreign
    try{
        foreach($name in @('materials','shadows','postprocess')){
            $images=@()
            foreach($round in @(1,2)){
                $image=Join-Path $outputPath ($name+'-'+$round+'.ppm')
                $log=Join-Path $outputPath ($name+'-'+$round+'.log')
                if((Invoke-EngineProgram $executable @('--language','en-US','--demo',$name,'--hidden','--frames','30','--capture',$image) $log) -ne 0){throw ('Player failed: '+$name+' / '+$round)}
                $bytes=[IO.File]::ReadAllBytes($image)
                if($bytes.Length -ne 2073615 -or [Text.Encoding]::ASCII.GetString($bytes,0,15) -ne "P6`n960 720`n255`n"){throw 'Unexpected capture format/dimensions.'}
                Assert-DemoLog $log
                $images+=((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant())
            }
            if($images[0] -ne $images[1]){throw ('Repeated image differs: '+$name)}
            $profile=Join-Path $outputPath ($name+'-profile')
            $log=Join-Path $outputPath ($name+'-profile.log')
            if((Invoke-EngineProgram $executable @('--language','en-US','--demo',$name,'--hidden','--frames','64','--profile',$profile) $log) -ne 0){throw ('Profiled Player failed: '+$name)}
            Assert-DemoLog $log
            $cpu=@(Import-Csv -LiteralPath (Join-Path $profile 'cpu.csv'))
            $gpu=@(Import-Csv -LiteralPath (Join-Path $profile 'gpu.csv'))
            if(@($cpu.frame|Sort-Object -Unique).Count -ne 64 -or $gpu.Count -ne 64){throw 'Missing actual CPU/GPU frames.'}
            $expectedDraws=if($name -eq 'shadows'){3}else{28}
            if(@($gpu|Where-Object{[int]$_.draws -ne $expectedDraws}).Count){throw 'Unexpected World draw count.'}
            $passCount=if($name -eq 'materials'){0}else{@(Import-Csv -LiteralPath (Join-Path $profile 'gpu-passes.csv')).Count}
            $expectedPasses=if($name -eq 'shadows'){320}elseif($name -eq 'postprocess'){704}else{0}
            if($passCount -ne $expectedPasses){throw 'Incomplete Pass timing rows.'}
            $results.Add([ordered]@{demo=$name;capture_frame=30;repeat_ppm_sha256=$images[0];captures_identical=$true;profile_frames=$gpu.Count;draws=$expectedDraws;pass_rows=$passCount})
        }
        $badLog=Join-Path $outputPath 'invalid-capture.log'
        if((Invoke-EngineProgram $executable @('--demo','materials','--hidden','--frames','29','--capture',(Join-Path $outputPath 'invalid.ppm')) $badLog) -ne 2 -or (Test-Path -LiteralPath (Join-Path $outputPath 'invalid.ppm'))){throw 'Invalid capture request was not rejected.'}
    }finally{Pop-Location}
}finally{$env:PATH=$oldPath}
$report=[ordered]@{schema=1;source_sha256=$manifest.source_sha256;git_head=$manifest.git_head;dirty=$manifest.dirty;restricted_system_path=$true;foreign_cwd=$true;source_hdr_present=$false;clean_machine_tested=$false;results=$results.ToArray()}
[IO.File]::WriteAllText((Join-Path $outputPath 'results.json'),($report|ConvertTo-Json -Depth 12)+"`n",[Text.UTF8Encoding]::new($false))
Write-Output ('Three graphics demos passed: '+$outputPath)
exit 0
