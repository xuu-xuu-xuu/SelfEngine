param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [Parameter(Mandatory=$true)][string]$Evidence,
    [string]$Package='',
    [ValidateRange(1,20)][int]$Rounds=20,
    [ValidateSet('All','Compile','Export','Lifecycle')][string]$Scenario='All',
    [switch]$Hidden
)
# 正式 supervisor / 命令 / 保存关闭路径验收。窗口自动化不是鼠标或真人验收。
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'EditorPackageRuntime.ps1')
$engine=if($Package){(Resolve-Path -LiteralPath $Package).Path}else{[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))}
if($Package){$Configuration='Release'}
if($Scenario -eq 'Export' -and !$Package){throw 'The export workflow requires the actual editor package.'}
$native=if($Package){$engine}else{Join-Path $engine ('out/build/windows-x64-'+$Configuration.ToLowerInvariant())}
$tool=Join-Path $(if($Package){Join-Path $engine 'Tools'}else{$native}) 'ProjectTool.exe'
$editor=Join-Path $native 'SelfEngineEditor.exe'
$powershell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$Evidence=[IO.Path]::GetFullPath($Evidence)
if(Test-Path -LiteralPath $Evidence){throw 'Use a new editor-workflow evidence directory.'}
New-Item -ItemType Directory -Path $Evidence|Out-Null
$utf8=New-Object Text.UTF8Encoding($false)
$result=[ordered]@{schemaVersion=1;success=$false;configuration=$Configuration;engineRoot=$engine;scenario=$Scenario;rounds=$Rounds;checks=@();runs=@();
    manualInputVerified=$false;visibleWindows=(!$Hidden);cleanMachineTested=$false;launcherInteractiveTimingTested=$false;gpuReferenceCountsMeasured=$false;managedAlcCountsMeasured=$false;error=''}
function JsonWrite([string]$Path,$Value){[IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 35),$utf8)}
function SaveResult {JsonWrite (Join-Path $Evidence 'workflow-tests.json') $result}
function Check([string]$Name,[bool]$Passed){$result.checks+=@([ordered]@{name=$Name;passed=$Passed});SaveResult;if(!$Passed){throw ('Editor workflow check failed: '+$Name)}}
function ReadJson([string]$Path){return Get-Content -LiteralPath $Path -Raw -Encoding UTF8|ConvertFrom-Json}
function Median($Values){$ordered=@($Values|Sort-Object);if(!$ordered.Count){return 0};return [double]$ordered[[int][Math]::Floor($ordered.Count/2)]}
function TreeDigest([string]$Directory){return (@(Get-ChildItem -LiteralPath $Directory -File -Recurse|Sort-Object FullName|ForEach-Object {$_.FullName.Substring($Directory.Length)+':'+(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}) -join "`n")}
function Create([string]$Name){
    $project=Join-Path $Evidence $Name
    $code=Invoke-EngineProgram $tool @('create',(Join-Path $engine 'Resources/Templates/EmptyGame'),$project) (Join-Path $Evidence ($Name+'-create.log'))
    Check ($Name+'_created') ($code -eq 0);return $project
}
function NewCase([string]$Project,[string]$Name,$Stages){
    $directory=Join-Path $Project ('out/Editor/workflow-smoke/'+$Name)
    New-Item -ItemType Directory -Path $directory|Out-Null
    $source=Get-Content -LiteralPath (Join-Path $Project 'Scripts/Spin.cs') -Raw -Encoding UTF8
    # 固定模板中的字段值仍为 1；改真正更新代码，避免字段初值被绑定覆盖后误判新行为。
    if(!$source.Contains('RadiansPerSecond*(float)seconds')){throw 'The Spin code fixture changed.'}
    $source=[regex]::Replace($source,'RadiansPerSecond\*\(float\)seconds(?:\*[123]f)?','RadiansPerSecond*(float)seconds')
    foreach($speed in @(1,2,3)){[IO.File]::WriteAllText((Join-Path $directory ('source-'+$speed+'.txt')),$source.Replace('RadiansPerSecond*(float)seconds',('RadiansPerSecond*(float)seconds*'+$speed+'f')),$utf8)}
    [IO.File]::WriteAllText((Join-Path $directory 'source-broken.txt'),$source+"`nThis is deliberately invalid C#;",$utf8)
    $config=Join-Path $directory 'workflow.json'
    JsonWrite $config ([ordered]@{schemaVersion=1;kind='SelfEngine.EditorWorkflowSmoke';projectRoot=$Project;stages=@($Stages)})
    return $config
}
function ProcessTree([int]$RootId,$Known){
    $entries=@(Get-CimInstance Win32_Process -Property ProcessId,ParentProcessId,Name -ErrorAction Stop)
    $ids=New-Object 'Collections.Generic.HashSet[int]';[void]$ids.Add($RootId)
    do{$added=$false;foreach($entry in $entries){if($ids.Contains([int]$entry.ParentProcessId)-and $ids.Add([int]$entry.ProcessId)){$added=$true}}}while($added)
    foreach($id in $ids){try{$observed=[Diagnostics.Process]::GetProcessById($id);$Known[[string]$id]=[ordered]@{id=$id;name=$observed.ProcessName;creationTicks=$observed.StartTime.ToUniversalTime().Ticks};$observed.Dispose()}catch{}}
}
function RunWorker([string]$Name,[string]$Project,[string]$Config,[bool]$NoSdk=$false){
    $arguments=@('--worker','--project',$Project,'--scene',(Join-Path $Project 'Scenes/Main.scene.json'),'--language','en-US','--no-ui-errors','--smoke-workflow',$Config)
    if($Hidden){$arguments+='--hidden'}
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$editor;$info.Arguments=(@($arguments|ForEach-Object {'"'+([regex]::Replace([regex]::Replace($_,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'}) -join ' ')
    $info.WorkingDirectory=$env:TEMP;$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.StandardOutputEncoding=[Text.Encoding]::UTF8;$info.StandardErrorEncoding=[Text.Encoding]::UTF8
    if($NoSdk){$info.EnvironmentVariables['PATH']=$env:SystemRoot+'\System32;'+$env:SystemRoot}
    $process=New-Object Diagnostics.Process;$process.StartInfo=$info
    $watch=[Diagnostics.Stopwatch]::StartNew();$known=@{};$samples=@();$lastTree=-1000;$code=-1;$started=$false
    try {
        [void]$process.Start();$started=$true;$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        while(!$process.WaitForExit(100)){
            if($watch.Elapsed.TotalMinutes -gt 40){throw ('Editor workflow timed out: '+$Name)}
            if($watch.ElapsedMilliseconds-$lastTree -ge 1000){
                $lastTree=$watch.ElapsedMilliseconds;ProcessTree $process.Id $known
                if(!$process.HasExited){try{$process.Refresh();$samples+=@([ordered]@{elapsedMs=$watch.ElapsedMilliseconds;handles=$process.HandleCount;privateBytes=$process.PrivateMemorySize64;workingSet=$process.WorkingSet64})}catch{if(!$process.HasExited){throw}}}
            }
        }
        $process.WaitForExit();$code=$process.ExitCode
        [IO.File]::WriteAllText((Join-Path $Evidence ($Name+'.log')),$stdout.Result+$stderr.Result,$utf8)
    } finally {
        if($started -and !$process.HasExited){& (Join-Path $env:SystemRoot 'System32/taskkill.exe') /PID $process.Id /T /F | Out-Null;[void]$process.WaitForExit(5000)}
        $process.Dispose();$watch.Stop()
    }
    $remaining=@(foreach($entry in $known.Values){try{$observed=[Diagnostics.Process]::GetProcessById($entry.id);if($observed.StartTime.ToUniversalTime().Ticks -eq $entry.creationTicks){$entry};$observed.Dispose()}catch{}})
    $run=[ordered]@{name=$Name;exitCode=$code;elapsedMs=$watch.ElapsedMilliseconds;restrictedPath=$NoSdk;trackedProcesses=@($known.Values);remainingProcesses=$remaining;samples=$samples;config=$Config}
    JsonWrite (Join-Path $Evidence ($Name+'-processes.json')) $run;$result.runs+=@($run);SaveResult
    Check ($Name+'_exit') ($code -eq 0);Check ($Name+'_no_remaining_processes') ($remaining.Count -eq 0)
    $settings=ReadJson $Config;$directory=Split-Path -Parent $Config;$reports=@()
    $supervisor=ReadJson (Join-Path $directory 'supervisor.json')
    Check ($Name+'_supervisor_completed') ($supervisor.success -and @($supervisor.stages).Count -eq @($settings.stages).Count)
    for($index=0;$index -lt @($settings.stages).Count;++$index){
        $report=ReadJson (Join-Path $directory ('stage-'+$index+'.json'));$reports+=@($report)
        Check ($Name+'_host_'+$index) ($report.success -and $report.managed_host -and $report.host_exit_code -eq 0 -and !$report.manual_input_verified)
        Check ($Name+'_candidate_preserved_'+$index) ($report.assembly_sha256 -ieq (Get-FileHash -LiteralPath $report.build_status.assembly -Algorithm SHA256).Hash)
        if($index -gt 0){
            $previous=$reports[$index-1]
            Check ($Name+'_new_pid_'+$index) ($report.process_id -ne $previous.process_id)
            Check ($Name+'_saved_session_'+$index) ([bool]$report.saved_scene_and_selection_restored)
            $same=$report.build_status.candidateRoot -eq $previous.build_status.candidateRoot
            Check ($Name+'_candidate_version_'+$index) $(if($settings.stages[$index].expectedBuild -eq 'stale'){$same}else{!$same})
        }
        if($index+1 -lt @($settings.stages).Count){Check ($Name+'_normal_compile_close_'+$index) ($report.dirty_scene_before_compile -and $report.scene_clean_after_compile -and $report.restart_published_after_close)}
        try{$alive=[Diagnostics.Process]::GetProcessById([int]$report.process_id);$alive.Dispose();Check ($Name+'_host_reaped_'+$index) $false}catch [ArgumentException]{}
    }
    # 构建锁文件可以留作同步锚点；验证可重新取得，而不以文件存在误判残留锁。
    $lockPath=if($Package){Join-Path $Project 'out/preview/preview.lock'}else{Join-Path $engine 'out/preview/preview.lock'}
    $lock=[IO.File]::Open($lockPath,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
    try{$lock.Lock(0,1);$lock.Unlock(0,1);$lock.Lock(1,1);$lock.Unlock(1,1)}finally{$lock.Dispose()}
    Check ($Name+'_build_lease_released') $true
    $identity=Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent ([string]$supervisor.stages[0].operation)))
    $globalUser=if($Package){Join-Path $env:LOCALAPPDATA ('SelfEngine/Editor/'+$identity)}else{Join-Path $engine ('out/Editor/User/'+$identity)}
    $projectLeaseFound=$false
    foreach($file in @(Get-ChildItem -LiteralPath (Join-Path $globalUser 'leases') -Filter '*.lock' -File)){
        $owner=ReadJson $file.FullName
        if([IO.Path]::GetFullPath([string]$owner.manifest) -ieq (Join-Path $Project 'project.json')){
            $probe=[IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$probe.Dispose();$projectLeaseFound=$true
        }
    }
    Check ($Name+'_project_lease_released') $projectLeaseFound
    Check ($Name+'_crash_markers_cleared') (@(Get-ChildItem -LiteralPath (Join-Path $Project 'out/Editor') -Filter process.json -File -Recurse).Count -eq 0)
    return [pscustomobject]@{Reports=$reports;Supervisor=$supervisor;Run=$run}
}
function PlayStage([double]$Speed,[string]$Patch='',[string]$Name='',[string]$Previous='', [int]$Cycles=1,[string]$Build='ready'){
    return [ordered]@{mode='playback';expectedBuild=$Build;radiansPerSecond=$Speed;playCycles=$Cycles;patchAfter=$Patch;sceneEditName=$Name;expectedSceneName=$Previous}
}
function ResourceTrend([string]$Name,$Samples,[string]$PrivateKey='private_bytes'){
    $values=@($Samples);$count=$values.Count;$edge=[Math]::Min(5,[int][Math]::Floor($count/2))
    if($edge -lt 1){return [ordered]@{name=$Name;sampleCount=$count;reviewed=$false}}
    $firstPrivate=Median @($values[0..($edge-1)]|ForEach-Object {$_.$PrivateKey});$lastPrivate=Median @($values[($count-$edge)..($count-1)]|ForEach-Object {$_.$PrivateKey})
    $firstHandles=Median @($values[0..($edge-1)]|ForEach-Object {$_.handles});$lastHandles=Median @($values[($count-$edge)..($count-1)]|ForEach-Object {$_.handles})
    # 这里只是资源增长报警阈值；原始序列保留，不能据此声称测到了 GPU/ALC 引用数。
    $growth=($lastPrivate-$firstPrivate -gt 16MB) -or ($lastHandles-$firstHandles -gt 4)
    return [ordered]@{name=$Name;sampleCount=$count;reviewed=$true;firstPrivateMedian=$firstPrivate;lastPrivateMedian=$lastPrivate;privateGrowthBytes=($lastPrivate-$firstPrivate);handleGrowth=($lastHandles-$firstHandles);growthSuspected=$growth;privateAlertBytes=16MB;handleAlert=4}
}
try {
    if($Package){[void](Read-EngineEditorPackage $engine);$result.packageManifestSha256=(Get-FileHash -LiteralPath (Join-Path $engine 'editor-package.json') -Algorithm SHA256).Hash}
    if($Scenario -in @('All','Compile')){
        $project=Create '编译重开 Project';$bindings=Join-Path $project 'Scripts/Main.bindings.json';$bindingHash=(Get-FileHash -LiteralPath $bindings -Algorithm SHA256).Hash
        $stages=@((PlayStage 1 'source-2.txt' 'Workflow saved 1'),(PlayStage 2 'source-broken.txt' 'Workflow saved 2' 'Workflow saved 1'),
            (PlayStage 2 'source-3.txt' 'Workflow saved 3' 'Workflow saved 2' 1 'stale'),(PlayStage 3 '' '' 'Workflow saved 3'))
        $run=RunWorker 'compile-reopen' $project (NewCase $project 'compile-reopen' $stages)
        Check 'syntax_failure_stops_play_until_authorized' ($run.Reports[2].stale_play_rejected -and $run.Reports[2].old_build_explicitly_authorized -and $run.Supervisor.stages[2].build_exit_code -ne 0)
        Check 'syntax_failure_has_file_line' ((Get-Content -LiteralPath $run.Reports[2].build_status.diagnosticsPath -Raw -Encoding UTF8) -match 'Spin\.cs\(\d+,\d+\).*error CS')
        Check 'bindings_unchanged_across_actual_code_changes' ($bindingHash -eq (Get-FileHash -LiteralPath $bindings -Algorithm SHA256).Hash)
    }
    if($Scenario -in @('All','Lifecycle')){
        $project=Create '连续使用 Project';$stages=@();$previous='';$speed=1
        for($round=0;$round -le $Rounds;++$round){
            $nextSpeed=if($round%2 -eq 0){2}else{3};$name='Lifecycle saved '+($round+1)
            if($round -eq $Rounds){$stages+=@(PlayStage $speed '' '' $previous $Rounds)}
            else{$stages+=@(PlayStage $speed ('source-'+$nextSpeed+'.txt') $name $previous);$previous=$name;$speed=$nextSpeed}
        }
        $life=RunWorker 'continuous-recompile' $project (NewCase $project 'continuous-recompile' $stages)
        Check 'requested_compile_rounds_completed' (@($life.Supervisor.stages|Where-Object restart_requested).Count -eq $Rounds)
        Check 'same_process_play_stop_rounds_completed' (@($life.Reports[-1].cycles).Count -eq $Rounds)
        $openTimes=@();$openResources=@()
        for($round=1;$round -le $Rounds;++$round){
            $name='open-close-'+$round;$opened=RunWorker $name $project (NewCase $project $name @((PlayStage $speed '' '' $previous)))
            $openTimes+=@($opened.Reports[0].open_elapsed_ms);$openResources+=@($opened.Reports[0].ready_resources)
        }
        $result.resourceTrends=@((ResourceTrend 'persistent_supervisor' @($life.Supervisor.stages|ForEach-Object resources)),
            (ResourceTrend 'same_process_play_stop' @($life.Reports[-1].cycles|ForEach-Object resources)),(ResourceTrend 'repeated_open_close' $openResources))
        Check 'resource_growth_review' (@($result.resourceTrends|Where-Object growthSuspected).Count -eq 0)
        $warm=@($openTimes|Select-Object -First 5)
        $result.performance=[ordered]@{openReadyTimesMs=$openTimes;warmMedianMs=(Median $warm);sampleCount=$warm.Count;budgetMs=10000;measurement='supervisor entry to registered editor commands after ten rendered frames; conservative ready bound; cached candidate';
            firstCompileMs=$life.Supervisor.stages[0].build_elapsed_ms;progressPolling=@($life.Supervisor.stages|Select-Object index,maximum_progress_poll_gap_ms,progress_poll_count);visibleProgressTimingClaimed=$false}
        if($warm.Count -ge 5){Check 'warm_project_open_budget' ($result.performance.warmMedianMs -gt 0 -and $result.performance.warmMedianMs -le 10000)}
    }
    if($Scenario -in @('All','Export') -and $Package){
        $project=Create '同包 GUI 导出 Project'
        $exportStage=[ordered]@{mode='export';expectedBuild='ready';exportName='WorkflowGame';expectedResult='succeeded'}
        $export=RunWorker 'package-gui-export' $project (NewCase $project 'export-success' @($exportStage)) $true
        $game=[string]$export.Reports[0].output_package;Check 'gui_export_uses_same_portable_package' ($export.Reports[0].export_report.source_mode -eq 'editor_package' -and (Test-Path -LiteralPath (Join-Path $game 'Start-Game.ps1')))
        $oldPath=$env:PATH;Push-Location -LiteralPath $env:TEMP
        try{$env:PATH=$env:SystemRoot+'\System32;'+$env:SystemRoot;$gameCode=Invoke-EngineProgram $powershell @('-NoProfile','-File',(Join-Path $game 'Start-Game.ps1'),'-Headless','-Frames','120','-Locale','en-US') (Join-Path $Evidence 'gui-export-game.log')}
        finally{$env:PATH=$oldPath;Pop-Location}
        Check 'gui_exported_game_runs_without_sdk' ($gameCode -eq 0)
        $gameDigest=TreeDigest $game
        $exportStage.expectedResult='failed';$conflict=RunWorker 'package-gui-conflict' $project (NewCase $project 'export-conflict' @($exportStage)) $true
        Check 'gui_same_name_keeps_original_game' ((TreeDigest $game) -ceq $gameDigest)
        $exportStage.exportName='WorkflowCancelled';$exportStage.expectedResult='cancelled'
        $cancelled=RunWorker 'package-gui-cancel' $project (NewCase $project 'export-cancel' @($exportStage)) $true
        $cancel=$cancelled.Reports[0]
        Check 'gui_cancel_after_real_staging' ($cancel.cancelled -and $cancel.cancel_stage_existed -and !(Test-Path -LiteralPath $cancel.cancel_stage) -and !(Test-Path -LiteralPath (Join-Path $project 'out/packages/WorkflowCancelled')))
        Check 'gui_cancel_enters_cancelling_within_budget' ($null -ne $cancel.cancelling_latency_ms -and $cancel.cancelling_latency_ms -le 1000)
        $result.exportCancellation=[ordered]@{stateLatencyMs=$cancel.cancelling_latency_ms;cleanupMs=$cancel.cancel_cleanup_ms;maximumUiFrameGapMs=$cancel.maximum_ui_frame_gap_ms}
    }
    elseif($Scenario -eq 'All'){$result.packageGuiExportTested=$false}
    if($Package){[void](Read-EngineEditorPackage $engine);Check 'portable_package_unchanged' ($result.packageManifestSha256 -eq (Get-FileHash -LiteralPath (Join-Path $engine 'editor-package.json') -Algorithm SHA256).Hash)}
    $result.success=$true;SaveResult;Write-Output ('Editor workflow verified: '+$Evidence);exit 0
}catch{$result.error=$_.Exception.Message;SaveResult;Write-Error $result.error -ErrorAction Continue;exit 1}
