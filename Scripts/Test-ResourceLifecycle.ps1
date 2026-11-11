param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$binary=(Resolve-Path -LiteralPath $Executable).Path
$root=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$cases=@([PSCustomObject]@{name='baseline';memory='placed';upload='batch';cache='on'},[PSCustomObject]@{name='committed';memory='committed';upload='batch';cache='on'},
    [PSCustomObject]@{name='individual';memory='placed';upload='individual';cache='on'},[PSCustomObject]@{name='uncached';memory='placed';upload='batch';cache='off'})
$imageHash=''
foreach($case in $cases){
    $directory=Join-Path $root $case.name
    $arguments=@('--light','--cycles','4','--warmup-cycles','2','--output',$directory,'--memory',$case.memory,'--upload',$case.upload,'--pso-cache',$case.cache)
    if((Invoke-EngineProgram $binary $arguments (Join-Path $root ($case.name+'.log'))) -ne 0){throw ('Resource smoke failed: '+$case.name)}
    $run=Get-Content -LiteralPath (Join-Path $directory 'run.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $rows=@(Import-Csv -LiteralPath (Join-Path $directory 'cycles.csv'));$gpu=@(Import-Csv -LiteralPath (Join-Path $directory 'gpu.csv'))
    if(-not $run.success -or -not $run.measurement_complete -or $rows.Count -ne 6 -or $gpu.Count -ne 6){throw 'Resource row coverage failed.'}
    $expectedPso=if($case.cache -eq 'on'){'1'}else{'4'};$expectedUploads=if($case.upload -eq 'batch'){'1'}else{'8'}
    foreach($row in $rows){
        if($row.pso_creations -ne $expectedPso -or $row.loaded_live_prepared -ne $expectedPso -or $row.upload_submissions -ne $expectedUploads){throw 'Native PSO/upload counters disagree.'}
        foreach($name in @('buffers','textures','pipelines','retiring','in_flight','live_prepared','live_programs')){if($row.('drained_'+$name) -ne '0'){throw ('Resource did not retire: '+$name)}}
    }
    $capture=Join-Path $root ($case.name+'-capture');$image=Join-Path $root ($case.name+'.ppm')
    $arguments=@('--light','--cycles','1','--warmup-cycles','0','--output',$capture,'--capture',$image,'--memory',$case.memory,'--upload',$case.upload,'--pso-cache',$case.cache)
    if((Invoke-EngineProgram $binary $arguments (Join-Path $root ($case.name+'-capture.log'))) -ne 0){throw 'Resource capture failed.'}
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$digest=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($image)))}finally{$sha.Dispose()}
    if($imageHash -and $digest -ne $imageHash){throw 'Resource policy changed full GPU image.'};$imageHash=$digest
}
$protected=Join-Path $root 'protected';New-Item -ItemType Directory -Path $protected | Out-Null
$sentinel=Join-Path $protected 'sentinel.txt';[IO.File]::WriteAllText($sentinel,'preserve-existing-output')
if((Invoke-EngineProgram $binary @('--light','--output',$protected) (Join-Path $root 'existing-output.log')) -eq 0){throw 'Existing output was accepted.'}
if([IO.File]::ReadAllText($sentinel) -ne 'preserve-existing-output' -or @(Get-ChildItem -LiteralPath $protected).Count -ne 1){throw 'Existing output was modified.'}
$invalid=Join-Path $root 'invalid-cycles'
if((Invoke-EngineProgram $binary @('--cycles','0','--output',$invalid) (Join-Path $root 'invalid-cycles.log')) -eq 0 -or (Test-Path -LiteralPath $invalid)){throw 'Invalid cycle count was not rejected before writing.'}
Write-Output ('Resource lifecycle smoke passed (four real GPU policy paths, full images, quiescence/Trim, output/failure guards): '+$root)
exit 0
