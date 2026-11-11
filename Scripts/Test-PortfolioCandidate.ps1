param([Parameter(Mandatory=$true)][string]$PackageDirectory,[Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$package=(Resolve-Path -LiteralPath $PackageDirectory).Path
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(-not $output.StartsWith($root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)){throw 'Use a fresh workspace evidence directory.'}
New-Item -ItemType Directory -Path $output | Out-Null
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
$manifest=Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
if($manifest.kind -ne 'PortfolioCandidate' -or $manifest.configuration -ne 'Release' -or $manifest.complete_v1){throw 'Expected a Release G3 candidate.'}
function Assert-Package{
    foreach($file in $manifest.files){
        $path=[IO.Path]::GetFullPath((Join-Path $package $file.path))
        if(-not $path.StartsWith($package.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or $file.path -match '(^|/)AGENTS\.md$'){throw 'Invalid delivery path.'}
        if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){throw ('Package input changed: '+$file.path)}
    }
}
Assert-Package
& python (Join-Path $PSScriptRoot 'Inspect-PeImports.py') --package $package --system (Join-Path $env:SystemRoot 'System32') --output (Join-Path $output 'pe-imports.json')
if($LASTEXITCODE){throw 'PE dependency closure failed.'}
$ps=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$oldPath=$env:PATH
$foreign=Join-Path $output 'foreign cwd with spaces';New-Item -ItemType Directory -Path $foreign | Out-Null
$results=[Collections.Generic.List[object]]::new()
function Run-Native([string]$Name,[string[]]$Arguments,[string]$Log,[int]$Expected=0){
    $code=Invoke-EngineProgram (Join-Path $package ($Name+'.exe')) $Arguments (Join-Path $output $Log)
    if($code -ne $Expected){throw ('Unexpected exit '+$Name+': '+$code+' / '+$Log)}
}
try{
    $env:PATH=(Join-Path $env:SystemRoot 'System32')+';'+$env:SystemRoot
    Push-Location -LiteralPath $foreign
    try{
        & $ps -NoProfile -File (Join-Path $PSScriptRoot 'Test-GraphicsDemo.ps1') -PackageDirectory $package -OutputDirectory (Join-Path $output 'graphics')
        if($LASTEXITCODE){throw 'G2 package regression failed.'}
        $gold=@{materials='5214ba7834b18ec113bb7aeec9803e6a7d4af6d1813312126bf40866f27153d5';shadows='91d3502568eb3ec137d07de3965be1ea5df6bdb4f14734b9650af94613b77469';postprocess='348d16df981684f60dbaacdeb1be2a00f54c2ee5645ed4e0d0af894725b3a7b1'}
        $graphics=Get-Content -LiteralPath (Join-Path $output 'graphics/results.json') -Raw -Encoding UTF8|ConvertFrom-Json
        foreach($row in $graphics.results){if($row.repeat_ppm_sha256 -ne $gold[$row.demo]){throw 'G2 fixed reference changed.'}}
        foreach($case in @(@('repeated','off','off','off','off',1000),@('repeated','off','off','on','on',1),@('mixed','off','on','on','on',48),@('mostly_outside','on','off','off','off',100))){
            $name=$case[0]+'-'+$case[4];$data=Join-Path $output $name
            $arguments=@('--config',(Join-Path $package ('Resources/Benchmark/'+$case[0]+'.json')),'--output',$data,'--source-id',$manifest.source_sha256,
                '--warmup','0.05','--seconds','0.2','--culling',$case[1],'--sorting',$case[2],'--binding-cache',$case[3],'--instancing',$case[4])
            Run-Native 'BenchmarkRunner' $arguments ($name+'.log')
            $run=Get-Content -LiteralPath (Join-Path $data 'run.json') -Raw -Encoding UTF8|ConvertFrom-Json
            $frames=@(Import-Csv -LiteralPath (Join-Path $data 'frames.csv'))
            if(-not $run.success -or -not $run.measurement_complete -or $run.debug_warnings -or $run.debug_errors -or -not $frames.Count){throw 'Pressure smoke failed.'}
            if(@($frames|Where-Object {[int]$_.draws -ne $case[5]}).Count){throw 'Actual optimized Draw count mismatch.'}
            $results.Add([ordered]@{mode=$name;frames=$frames.Count;draws=$case[5]})
        }
        & $ps -NoProfile -File (Join-Path $PSScriptRoot 'Test-ResourceLifecycle.ps1') -Executable (Join-Path $package 'ResourceLifecycleBenchmark.exe') -OutputDirectory (Join-Path $output 'resource-policies')
        if($LASTEXITCODE){throw 'Resource policy closure failed.'}
        & $ps -NoProfile -File (Join-Path $PSScriptRoot 'Test-ResourceLoading.ps1') -Executable (Join-Path $package 'ResourceLoading.exe') -OutputDirectory (Join-Path $output 'loading-lifecycle')
        if($LASTEXITCODE){throw 'Loading cancellation/exit closure failed.'}
        $loadingHash=''
        foreach($mode in @('sync','async')){
            $image=Join-Path $output ($mode+'.ppm');$data=Join-Path $output ('loading-full-'+$mode)
            Run-Native 'ResourceLoading' @('--workload',(Join-Path $package 'Resources/Loading'),'--output',$data,'--source-id',$manifest.source_sha256,'--mode',$mode,'--frames','480','--capture',$image) ('loading-full-'+$mode+'.log')
            $hash=(Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant()
            if(($loadingHash -and $hash -ne $loadingHash) -or $hash -ne '019e05bbb7f3c3d9497f0ac53a399dc2f91d3c4eba28cab6d554fdf3ce9c4edf'){throw 'Packaged loading full reference differs.'};$loadingHash=$hash
        }
        $editorProject=Join-Path $output 'editor-project';New-Item -ItemType Directory -Path $editorProject | Out-Null
        Run-Native 'EditorSandbox' @('--demo','--project',$editorProject,'--language','zh-CN','--hidden','--frames','30','--capture',(Join-Path $output 'editor.ppm')) 'editor.log'
        foreach($demo in @('materials','shadows','postprocess','repeated','mixed','mostly_outside','resources','loading-sync','loading-async','editor')){
            $launcherFrames=if($demo -like 'loading-*'){'480'}else{'30'}
            $arguments=@('-NoProfile','-File',(Join-Path $package 'Scripts/Run-Portfolio.ps1'),'-Demo',$demo,'-Headless','-Frames',$launcherFrames,'-Count','32','-Warmup','0.05','-Seconds','0.2')
            if((Invoke-EngineProgram $ps $arguments (Join-Path $output ('launcher-'+$demo+'.log'))) -ne 0){throw ('Packaged launcher failed: '+$demo)}
        }
    }finally{Pop-Location}
}finally{$env:PATH=$oldPath}
Assert-Package
Write-EngineJson (Join-Path $output 'results.json') ([ordered]@{schema=1;success=$true;build_id=$manifest.build_id;source_sha256=$manifest.source_sha256;restricted_system_path=$true;foreign_cwd=$true;pe_imports_verified=$true;app_local_crt=$true;clean_machine_tested=$false;g2_references_equal=$true;loading_reference_sha256=$loadingHash;launchers=10;pressure=$results.ToArray();package_inputs_unchanged=$true})
Write-Output ('PortfolioCandidate integration passed: '+$output)
exit 0
