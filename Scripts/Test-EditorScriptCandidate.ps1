param(
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [Parameter(Mandatory=$true)][string]$Evidence,
    [string]$Package=''
)
# 真实 .NET 编译与候选复用/回退验收；不创建 GPU 窗口，由协调者串行运行。
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$engine=if($Package){(Resolve-Path -LiteralPath $Package).Path}else{[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))}
if($Package){$Configuration='Release'}
$native=if($Package){Join-Path $engine 'Tools'}else{Join-Path $engine ('out/build/windows-x64-'+$Configuration.ToLowerInvariant())}
$Evidence=[IO.Path]::GetFullPath($Evidence)
if(Test-Path -LiteralPath $Evidence){throw 'Use a new candidate evidence directory.'}
New-Item -ItemType Directory -Path $Evidence|Out-Null
$project=Join-Path $Evidence '脚本 Project'
$powershell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$result=[ordered]@{schemaVersion=1;success=$false;configuration=$Configuration;engineRoot=$engine;projectRoot=$project;checks=@();gpuTested=$false;error=''}
$utf8=New-Object Text.UTF8Encoding($false)
function SaveResult {[IO.File]::WriteAllText((Join-Path $Evidence 'candidate-tests.json'),($result|ConvertTo-Json -Depth 10),$utf8)}
function Check([string]$Name,[bool]$Passed){$result.checks+=@([ordered]@{name=$Name;passed=$Passed});SaveResult;if(!$Passed){throw ('Editor candidate check failed: '+$Name)}}
function Candidate([string]$Name){
    $operation=Join-Path $project ('out/Editor/contract/operations/'+$Name)
    New-Item -ItemType Directory -Path $operation -Force|Out-Null
    $code=Invoke-EngineProgram $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $engine 'Scripts/Build-EditorScriptCandidate.ps1'),
        '-ProjectRoot',$project,'-NativeDirectory',$native,'-OperationDirectory',$operation,'-Configuration',$Configuration,'-Locale','en-US') (Join-Path $operation 'build.log')
    $status=Get-Content -LiteralPath (Join-Path $operation 'build-status.json') -Raw -Encoding UTF8|ConvertFrom-Json
    return [pscustomobject]@{ExitCode=$code;Status=$status;Operation=$operation}
}
$previousPath=$env:PATH
try {
    $code=Invoke-EngineProgram (Join-Path $native 'ProjectTool.exe') @('create',(Join-Path $engine 'Resources/Templates/EmptyGame'),$project) (Join-Path $Evidence 'create.log')
    Check 'create_actual_project' ($code -eq 0)
    if($Package){$env:PATH=$env:SystemRoot+'\System32;'+$env:SystemRoot}
    $first=Candidate '01-first'
    Check 'first_candidate_ready' ($first.ExitCode -eq 0 -and $first.Status.state -eq 'ready' -and $first.Status.hasCandidate)
    Check 'all_behaviour_catalogued' (@((Get-Content -LiteralPath (Join-Path $first.Status.candidateRoot 'script-types.json') -Raw -Encoding UTF8|ConvertFrom-Json).types) -contains 'emptygame.spin')
    $firstHash=(Get-FileHash -LiteralPath $first.Status.assembly -Algorithm SHA256).Hash
    $reuse=Candidate '02-reuse'
    Check 'same_source_reuses_frozen_candidate' ($reuse.ExitCode -eq 0 -and $reuse.Status.candidateRoot -eq $first.Status.candidateRoot)
    $bindings=Join-Path $project 'Scripts/Main.bindings.json'
    [IO.File]::AppendAllText($bindings,"`n",$utf8)
    $bindingsOnly=Candidate '03-bindings-only'
    Check 'binding_save_does_not_recompile_csharp' ($bindingsOnly.ExitCode -eq 0 -and $bindingsOnly.Status.candidateRoot -eq $first.Status.candidateRoot)
    $env:PATH=$previousPath
    $source=Join-Path $project 'Scripts/Spin.cs';$original=Get-Content -LiteralPath $source -Raw -Encoding UTF8
    if(!$original.Contains('RadiansPerSecond=1')){throw 'Spin fixture changed.'}
    [IO.File]::WriteAllText($source,$original.Replace('RadiansPerSecond=1','RadiansPerSecond=2'),$utf8)
    $second=Candidate '04-changed'
    Check 'changed_source_has_new_candidate' ($second.ExitCode -eq 0 -and $second.Status.candidateRoot -ne $first.Status.candidateRoot -and $second.Status.candidateIdentity -ne $first.Status.candidateIdentity)
    $secondHash=(Get-FileHash -LiteralPath $second.Status.assembly -Algorithm SHA256).Hash
    Check 'actual_script_assembly_changed' ($secondHash -ne $firstHash)
    Check 'first_candidate_unchanged' ($firstHash -eq (Get-FileHash -LiteralPath $first.Status.assembly -Algorithm SHA256).Hash)
    [IO.File]::AppendAllText($source,"`nThis is deliberately invalid C#;",$utf8)
    $broken=Candidate '05-broken'
    Check 'compile_error_returns_stale_candidate' ($broken.ExitCode -ne 0 -and $broken.Status.state -eq 'stale' -and $broken.Status.candidateRoot -eq $second.Status.candidateRoot)
    Check 'compiler_keeps_file_line_code' ((Get-Content -LiteralPath (Join-Path $broken.Operation 'build.log') -Raw -Encoding UTF8) -match 'Spin\.cs\(\d+,\d+\).*error CS')
    Check 'failure_keeps_last_assembly' ($secondHash -eq (Get-FileHash -LiteralPath $second.Status.assembly -Algorithm SHA256).Hash)
    [IO.File]::WriteAllText($source,$original.Replace('RadiansPerSecond=1','RadiansPerSecond=3'),$utf8)
    $env:PATH=$env:SystemRoot+'\System32;'+$env:SystemRoot
    $missingSdk=Candidate '06-missing-sdk'
    Check 'missing_sdk_keeps_last_candidate' ($missingSdk.ExitCode -ne 0 -and $missingSdk.Status.state -eq 'stale' -and $missingSdk.Status.diagnosticKey -eq 'Export.SdkMissing' -and $missingSdk.Status.candidateRoot -eq $second.Status.candidateRoot)
    $pointer=Get-Content -LiteralPath (Join-Path $project ('out/Editor/contract/last-success-'+$Configuration.ToLowerInvariant()+'.json')) -Raw -Encoding UTF8|ConvertFrom-Json
    Check 'only_valid_candidate_published' ($pointer.candidateRoot -eq $second.Status.candidateRoot)
    $result.success=$true;SaveResult
    Write-Output ('Editor script candidate checks passed: '+$Evidence)
    exit 0
}catch{$result.error=$_.Exception.Message;SaveResult;Write-Error $result.error -ErrorAction Continue;exit 1}
finally{$env:PATH=$previousPath}
