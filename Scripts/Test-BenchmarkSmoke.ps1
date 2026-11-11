param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$Configurations,[Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
$binary=(Resolve-Path -LiteralPath $Executable).Path
$configs=(Resolve-Path -LiteralPath $Configurations).Path
$root=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
foreach($name in @('repeated','mixed')){
    $directory=Join-Path $root $name
    if((Invoke-EngineProgram $binary @('--config',(Join-Path $configs ($name+'.json')),'--output',$directory,'--count','32','--warmup','0.05','--seconds','0.2') (Join-Path $root ($name+'.log'))) -ne 0){throw ('Benchmark smoke failed: '+$name)}
    $run=Get-Content -LiteralPath (Join-Path $directory 'run.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $frames=@(Import-Csv -LiteralPath (Join-Path $directory 'frames.csv'));$gpu=@(Import-Csv -LiteralPath (Join-Path $directory 'gpu.csv'))
    if(-not $run.success -or -not $run.measurement_complete -or $run.object_count -ne 32 -or $run.rendered_frames -ne $gpu.Count -or $run.cpu_frames_streamed -ne $gpu.Count){throw 'Incomplete streamed frames.'}
    if(@($frames|Where-Object{$_.candidates -ne '32' -or $_.visible -ne '32' -or $_.draws -ne '32'}).Count -or -not @($frames|Where-Object{$_.phase -eq 'sample'}).Count){throw 'Invalid benchmark workload counts or sampling window.'}
    $image=Join-Path $root ($name+'.ppm');$captured=Join-Path $root ($name+'-capture')
    if((Invoke-EngineProgram $binary @('--config',(Join-Path $configs ($name+'.json')),'--output',$captured,'--count','32','--capture',$image) (Join-Path $root ($name+'-capture.log'))) -ne 0 -or -not (Test-Path -LiteralPath $image)){throw 'Benchmark capture failed.'}
    $capture=Get-Content -LiteralPath (Join-Path $captured 'run.json') -Raw -Encoding UTF8|ConvertFrom-Json
    if($capture.mode -ne 'capture' -or $capture.measurement_complete -or $capture.layout_sha256 -ne $run.layout_sha256){throw 'Capture must preserve layout and stay separate from measurement.'}
}
Write-Output ('Benchmark smoke passed (short smoke only): '+$root)
exit 0
