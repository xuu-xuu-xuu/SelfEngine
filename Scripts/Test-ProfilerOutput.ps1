param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
& $Executable --language en-US --hidden --frames 64 --profile $OutputDirectory
if($LASTEXITCODE -ne 0){throw 'Profiled Player failed.'}
$cpu=Import-Csv -LiteralPath (Join-Path $OutputDirectory 'cpu.csv')
$gpu=Import-Csv -LiteralPath (Join-Path $OutputDirectory 'gpu.csv')
$frames=@($cpu | Where-Object {$_.scope -eq 'CPU.Frame'})
if($frames.Count -ne 64 -or @($gpu).Count -ne 64){throw 'Expected 64 actual CPU/GPU frames.'}
foreach($scope in @('CPU.Update','CPU.Extract','CPU.CommandPrepare','CPU.CommandRecord','CPU.SubmitPresent')){
    if(@($cpu | Where-Object {$_.scope -eq $scope}).Count -ne 64){throw "Missing per-frame scope: $scope"}
}
for($index=0;$index -lt 64;++$index){
    if([uint64]$gpu[$index].source_frame -ne ($index+1) -or [uint64]$gpu[$index].draws -ne 2 -or [uint64]$gpu[$index].frequency_hz -eq 0){
        throw 'GPU rows must identify the actual Player frame, two cubes and a nonzero queue frequency.'
    }
}
Write-Output 'Profiled Player CSV: 64 CPU/GPU frames, five CPU scopes, source IDs and two real cube draws passed.'
