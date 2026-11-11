param(
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [Parameter(Mandatory=$true)][string]$Evidence,
    [string]$Package=''
)
# 经正式 Worker 和真实窗口宿主调用统一命令；不是鼠标/真人验收。
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$engine=if($Package){(Resolve-Path -LiteralPath $Package).Path}else{[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))}
if($Package){$Configuration='Release'}
$native=if($Package){$engine}else{Join-Path $engine ('out/build/windows-x64-'+$Configuration.ToLowerInvariant())}
$tool=Join-Path $(if($Package){Join-Path $engine 'Tools'}else{$native}) 'ProjectTool.exe'
$editor=Join-Path $native 'SelfEngineEditor.exe'
$Evidence=[IO.Path]::GetFullPath($Evidence)
if(Test-Path -LiteralPath $Evidence){throw 'Use a new shared-smoke evidence directory.'}
New-Item -ItemType Directory -Path $Evidence|Out-Null
$utf8=New-Object Text.UTF8Encoding($false)
$result=[ordered]@{schemaVersion=1;success=$false;configuration=$Configuration;engineRoot=$engine;checks=@();realWindowHost=$true;manualInputVerified=$false;error=''}
function SaveResult {[IO.File]::WriteAllText((Join-Path $Evidence 'shared-smoke-tests.json'),($result|ConvertTo-Json -Depth 10),$utf8)}
function Check([string]$Name,[bool]$Passed){$result.checks+=@([ordered]@{name=$Name;passed=$Passed});SaveResult;if(!$Passed){throw ('Shared editor smoke check failed: '+$Name)}}
function Create([string]$Name,[string]$Template){
    $project=Join-Path $Evidence $Name
    $code=Invoke-EngineProgram $tool @('create',(Join-Path $engine ('Resources/Templates/'+$Template)),$project) (Join-Path $Evidence ($Name+'-create.log'))
    Check ($Name+'_created') ($code -eq 0);return $project
}
function Playback([string]$Name,[string]$Project){
    $report=Join-Path $Evidence ($Name+'.json')
    $code=Invoke-EngineProgram $editor @('--worker','--project',$Project,'--scene',(Join-Path $Project 'Scenes/Main.scene.json'),'--language','en-US','--hidden','--smoke-playback','--smoke-report',$report) (Join-Path $Evidence ($Name+'.log'))
    Check ($Name+'_report_written') (Test-Path -LiteralPath $report -PathType Leaf)
    return [pscustomobject]@{ExitCode=$code;Report=(Get-Content -LiteralPath $report -Raw -Encoding UTF8|ConvertFrom-Json)}
}
function CheckPlayback([string]$Name,$Value,[bool]$Managed,[bool]$Spin){
    $report=$Value.Report
    Check ($Name+'_passed') ($Value.ExitCode -eq 0 -and $report.success -and $report.host_exit_code -eq 0)
    Check ($Name+'_correct_host') ($report.managed_host -eq $Managed)
    Check ($Name+'_single_step_and_pause') ($report.fixed_step_after -eq $report.fixed_step_before+1 -and $report.paused_after_step -and $report.paused_wait_stable -and @($report.paused_wait_samples).Count -ge 5 -and @($report.after_step_samples).Count -ge 4)
    Check ($Name+'_edit_state_unchanged') ($report.edit_snapshot_unchanged -and $report.selection_unchanged -and $report.edit_history_unchanged -and $report.play_world_released)
    Check ($Name+'_actual_command_routes') (($report.commands.id -join ',') -eq 'editor.play,editor.pause,editor.step,editor.stop' -and $report.command_driver -and !$report.manual_input_verified)
    Check ($Name+'_spin_scope') ($report.spin.applicable -eq $Spin)
    if($Spin){Check ($Name+'_csharp_spin_executed') ($report.spin.changed -and $report.spin.before.alive -and $report.spin.after.alive -and $report.spin.after.script_components -gt 0)}
}
try {
    $blank=Create 'Native Project' 'BlankScene'
    $managed=Create '托管 Project' 'EmptyGame'
    CheckPlayback 'native' (Playback 'native' $blank) $false $false
    CheckPlayback 'managed' (Playback 'managed' $managed) $true $true
    # 去掉启用相机后必须在正式 Play 校验处失败，而不是绕过校验开始模拟。
    $scene=Join-Path $blank 'Scenes/Main.scene.json';$document=Get-Content -LiteralPath $scene -Raw -Encoding UTF8|ConvertFrom-Json
    foreach($entity in $document.entities){if($entity.components.PSObject.Properties.Name -contains 'camera'){$entity.components.PSObject.Properties.Remove('camera')}}
    [IO.File]::WriteAllText($scene,($document|ConvertTo-Json -Depth 20),$utf8)
    $noCamera=Playback 'no-camera' $blank
    Check 'missing_camera_rejected_by_command' ($noCamera.ExitCode -ne 0 -and !$noCamera.Report.success -and $noCamera.Report.detail -eq 'Editor.PlayNoCamera' -and $noCamera.Report.commands[0].id -eq 'editor.play' -and !$noCamera.Report.commands[0].succeeded)
    # 零速 Spin 仍会走托管单步，但没有旋转；负控证明 fixed index 不能代替 C# 行为证据。
    $bindings=Join-Path $managed 'Scripts/Main.bindings.json';$document=Get-Content -LiteralPath $bindings -Raw -Encoding UTF8|ConvertFrom-Json
    foreach($script in $document.Scripts){if($script.Type -eq 'emptygame.spin'){$script.Fields.RadiansPerSecond.Value=0}}
    [IO.File]::WriteAllText($bindings,($document|ConvertTo-Json -Depth 20),$utf8)
    $noSpin=Playback 'no-spin-motion' $managed
    Check 'unchanged_spin_cannot_pass_on_clock_alone' ($noSpin.ExitCode -ne 0 -and !$noSpin.Report.success -and $noSpin.Report.spin.applicable -and !$noSpin.Report.spin.changed -and $noSpin.Report.fixed_step_after -eq $noSpin.Report.fixed_step_before+1 -and $noSpin.Report.failure_check -eq 'spin_transform_unchanged')
    $result.success=$true;SaveResult;Write-Output ('Shared native/managed editor smoke verified: '+$Evidence);exit 0
}catch{$result.error=$_.Exception.Message;SaveResult;Write-Error $result.error -ErrorAction Continue;exit 1}
