param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$template = Join-Path $repo 'Resources/Templates/BlankScene'
$project = Join-Path $OutputDirectory 'project'
$toolkitCapture = Join-Path $OutputDirectory 'toolkit.ppm'
$imguiCapture = Join-Path $OutputDirectory 'imgui.ppm'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
if (Test-Path -LiteralPath $project) { Remove-Item -Recurse -Force $project }
Copy-Item -Recurse $template $project

function Run-Editor([string[]]$arguments, [string]$logName) {
    $stdout = Join-Path $OutputDirectory "$logName.stdout.log"
    $stderr = Join-Path $OutputDirectory "$logName.stderr.log"
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru -Wait `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if ($process.ExitCode -ne 0) { throw "$logName exit $($process.ExitCode)" }
}

Run-Editor @('--worker','--project',$project,'--editor-ui=toolkit','--hidden','--frames','2') 'toolkit-hidden'
Run-Editor @('--worker','--project',$project,'--editor-ui=toolkit','--frames','3','--capture',$toolkitCapture) 'toolkit-capture'
Run-Editor @('--worker','--project',$project,'--editor-ui=imgui','--frames','3','--capture',$imguiCapture) 'imgui-capture'

foreach ($capture in @($toolkitCapture, $imguiCapture)) {
    if (-not (Test-Path -LiteralPath $capture)) { throw "missing capture $capture" }
    $bytes = [IO.File]::ReadAllBytes($capture)
    $header = [Text.Encoding]::ASCII.GetString($bytes, 0, [Math]::Min(32, $bytes.Length))
    if (-not $header.StartsWith("P6`n1280 760`n")) { throw "unexpected PPM header $capture" }
}

$report = @{
    success = $true
    toolkitHidden = $true
    toolkitCapture = $true
    imguiCapture = $true
    width = 1280
    height = 760
}
$report | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
Write-Output 'EditorToolkitSmoke passed (toolkit hidden/capture, imgui capture).'
