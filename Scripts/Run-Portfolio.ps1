param(
    [Parameter(Mandatory=$true)][ValidateSet('materials','shadows','postprocess','repeated','mixed','mostly_outside','resources','loading-sync','loading-async','editor')][string]$Demo,
    [switch]$Headless,[int]$Frames=0,[int]$Count=1000,[double]$Seconds=60,[double]$Warmup=5,
    [ValidateSet('on','off')][string]$Culling='off',[ValidateSet('on','off')][string]$Sorting='off',
    [ValidateSet('on','off')][string]$BindingCache='off',[ValidateSet('on','off')][string]$Instancing='off',
    [ValidateSet('placed','committed')][string]$Memory='placed',[ValidateSet('batch','individual')][string]$Upload='batch',
    [ValidateSet('on','off')][string]$PsoCache='on',[int]$BudgetBytes=262144,[int]$BudgetTasks=4)
$ErrorActionPreference='Stop'
$package=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(-not (Test-Path -LiteralPath (Join-Path $package 'manifest.json'))){throw 'Run this launcher from an exported PortfolioCandidate.'}
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$manifest=Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
$run=Join-Path $package ('Runs/'+$Demo+'-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$arguments=@()
if($Demo -in @('materials','shadows','postprocess')){
    $executable='EngineSandbox.exe';$arguments=@('--demo',$Demo,'--language','zh-CN')
    if($Headless){$arguments+='--hidden'};if($Frames){$arguments+=@('--frames',"$Frames")}
    if($Headless -and -not $Frames){throw 'Headless gallery requires -Frames.'}
}elseif($Demo -in @('repeated','mixed','mostly_outside')){
    $executable='BenchmarkRunner.exe';$arguments=@('--config',(Join-Path $package ('Resources/Benchmark/'+$Demo+'.json')),'--output',(Join-Path $run 'data'),
        '--source-id',$manifest.source_sha256,'--count',"$Count",'--warmup',$Warmup.ToString([Globalization.CultureInfo]::InvariantCulture),
        '--seconds',$Seconds.ToString([Globalization.CultureInfo]::InvariantCulture),'--culling',$Culling,'--sorting',$Sorting,'--binding-cache',$BindingCache,'--instancing',$Instancing)
    if(-not $Headless){$arguments+='--show'}
}elseif($Demo -eq 'resources'){
    $executable='ResourceLifecycleBenchmark.exe';$arguments=@('--output',(Join-Path $run 'data'),'--source-id',$manifest.source_sha256,
        '--memory',$Memory,'--upload',$Upload,'--pso-cache',$PsoCache,'--warmup-cycles','4','--cycles','24')
}elseif($Demo -eq 'editor'){
    $executable='EditorSandbox.exe';$arguments=@('--demo','--project',$run,'--language','zh-CN')
    if($Headless){$arguments+='--hidden'};if($Frames){$arguments+=@('--frames',"$Frames")}
    if($Headless -and -not $Frames){throw 'Headless editor requires -Frames.'}
}else{
    $executable='ResourceLoading.exe';$strategy=$Demo.Substring(8);if(-not $Frames){$Frames=480}
    $arguments=@('--workload',(Join-Path $package 'Resources/Loading'),'--output',(Join-Path $run 'data'),'--source-id',$manifest.source_sha256,
        '--mode',$strategy,'--language','zh-CN','--frames',"$Frames",'--budget-bytes',"$BudgetBytes",'--budget-tasks',"$BudgetTasks")
    if(-not $Headless){$arguments+='--show'}
}
$invocation=[ordered]@{schema=1;demo=$Demo;executable=$executable;arguments=$arguments;source_sha256=$manifest.source_sha256;formal_protocol=$false}
[IO.File]::WriteAllText((Join-Path $run 'invocation.json'),($invocation|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Push-Location -LiteralPath $run
try{$result=Invoke-EngineProgram (Join-Path $package $executable) $arguments (Join-Path $run 'console.log')}finally{Pop-Location}
Write-Output ('Output: '+$run)
exit $result
