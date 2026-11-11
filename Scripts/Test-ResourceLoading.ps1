param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$binary=(Resolve-Path -LiteralPath $Executable).Path
$root=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$workload=Join-Path $root 'workload'
if((Invoke-EngineProgram $binary @('--prepare-workload',$workload,'--light') (Join-Path $root 'prepare.log')) -ne 0){throw 'Loading workload preparation failed.'}
$imageHash=''
foreach($mode in @('sync','async')){
    foreach($capture in @($false,$true)){
        $name=$mode+$(if($capture){'-capture'}else{'-measure'});$directory=Join-Path $root $name
        $arguments=@('--workload',$workload,'--output',$directory,'--mode',$mode,'--frames','96','--trigger-frame','8','--frame-ms','0','--budget-bytes','4096','--budget-tasks','2')
        $image=Join-Path $root ($mode+'.ppm');if($capture){$arguments+=@('--capture',$image)}
        if((Invoke-EngineProgram $binary $arguments (Join-Path $root ($name+'.log'))) -ne 0){throw ('Loading smoke failed: '+$name)}
        $run=Get-Content -LiteralPath (Join-Path $directory 'run.json') -Raw -Encoding UTF8|ConvertFrom-Json
        $rows=@(Import-Csv -LiteralPath (Join-Path $directory 'frames.csv'));$gpu=@(Import-Csv -LiteralPath (Join-Path $directory 'gpu.csv'))
        if(-not $run.success -or $run.measurement_complete -eq $capture -or $rows.Count -ne 96 -or $gpu.Count -ne 96 -or $run.objects -ne 4 -or $run.epochs -ne 1){throw 'Loading coverage/mode mismatch.'}
        if($run.debug_warnings -or $run.debug_errors -or $run.final_retiring -or $run.final_in_flight -or $run.baseline_memory_used -ne $run.final_memory_used){throw 'Loading retirement/diagnostics mismatch.'}
        foreach($row in $rows){
            if($row.draws -ne '4' -or $row.debug_warnings -ne '0' -or $row.debug_errors -ne '0' -or $row.pipeline_creations -ne '0' -or $row.shader_compilations -ne '0'){throw 'Loading frame diagnostics/draw mismatch.'}
            if($mode -eq 'async' -and ([uint64]$row.staged_footprint_bytes -gt 4096 -or [int]$row.copy_tasks -gt 2 -or $row.staged_footprint_bytes -ne $row.native_staged_footprint -or $row.copy_tasks -ne $row.native_staged_copies)){throw 'Actual upload budget mismatch.'}
        }
        if($capture){$sha=[Security.Cryptography.SHA256]::Create();try{$digest=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($image)))}finally{$sha.Dispose()}
            if($imageHash -and $digest -ne $imageHash){throw 'Sync/async final complete GPU images differ.'};$imageHash=$digest}
    }
}
$directory=Join-Path $root 'scene-switch'
$arguments=@('--workload',$workload,'--output',$directory,'--mode','async','--frames','128','--trigger-frame','8','--frame-ms','0','--budget-bytes','4096','--budget-tasks','2','--cancel-frame','20','--restart-frame','28')
if((Invoke-EngineProgram $binary $arguments (Join-Path $root 'scene-switch.log')) -ne 0){throw 'Loading scene-switch smoke failed.'}
$run=Get-Content -LiteralPath (Join-Path $directory 'run.json') -Raw -Encoding UTF8|ConvertFrom-Json
if(-not $run.success -or $run.epochs -ne 2 -or $run.final_retiring -or $run.final_in_flight){throw 'Scene switch did not quiesce.'}
$directory=Join-Path $root 'early-exit'
$arguments=@('--workload',$workload,'--output',$directory,'--mode','async','--frames','25','--trigger-frame','8','--frame-ms','0','--budget-bytes','512','--budget-tasks','1')
if((Invoke-EngineProgram $binary $arguments (Join-Path $root 'early-exit.log')) -eq 0){throw 'Incomplete resource run incorrectly passed.'}
$run=Get-Content -LiteralPath (Join-Path $directory 'run.json') -Raw -Encoding UTF8|ConvertFrom-Json
if($run.success -or $run.measurement_complete -or $run.final_retiring -or $run.final_in_flight -or $run.baseline_memory_used -ne $run.final_memory_used){throw 'Early loading exit did not retire safely.'}
Write-Output ('Resource loading smoke passed: real running scene, source identity, two strategies, full images, scene switch and early retirement: '+$root)
exit 0
