param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$TimeoutSeconds = 300,
    [switch]$Cancel,
    [switch]$Flow
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $OutputDirectory 'project'
if (Test-Path -LiteralPath $project) { Remove-Item -Recurse -Force $project }
Copy-Item -Recurse (Join-Path $repo 'Resources/Templates/EmptyGame') $project
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
$env:SELFENGINE_EDITOR_AUTO_EXPORT = '1'
$env:SELFENGINE_EDITOR_OPEN_PANEL = 'build-export'
if ($Cancel -or $Flow) { $env:SELFENGINE_EDITOR_AUTO_EXPORT_CANCEL_MS = '1500' }
if ($Flow) { $env:SELFENGINE_EDITOR_AUTO_EXPORT_FLOW = '1' }
$process = Start-Process -FilePath $Executable -ArgumentList @('--worker','--project',$project,'--editor-ui=toolkit','--hidden','--frames','100000') -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try {
    $started = $false
    $finished = $false
    $afterStart = ''
    for ($wait = 0; $wait -lt $TimeoutSeconds; ++$wait) {
        if (Test-Path -LiteralPath $stdout) {
            $log = Get-Content -LiteralPath $stdout -Raw
            if ($log -match 'EditorUi\.ToolkitExport: start') {
                $started = $true
                $afterStart = $log.Substring($log.LastIndexOf('EditorUi.ToolkitExport: start'))
                if ($Flow) {
                    if ($log -match 'EditorUi\.ToolkitExport: finished cancelled' -and
                        $log -match 'EditorUi\.ToolkitExport: finished succeeded') { $finished = $true; break }
                }
                elseif ($afterStart -match 'EditorUi\.ToolkitExport: finished') { $finished = $true; break }
            }
        }
        Start-Sleep -Seconds 1
    }
    if (-not $started) { throw 'auto export did not start' }
    if (-not $finished) { throw "auto export did not finish in $TimeoutSeconds s" }
    if ($Flow) {
        $log = Get-Content -LiteralPath $stdout -Raw
        if ($log -notmatch 'EditorUi\.ToolkitExport: finished cancelled') { throw 'flow cancel missing' }
        if ($log -notmatch 'EditorUi\.ToolkitExport: finished succeeded') { throw 'flow success missing' }
        $packages = Join-Path $repo '.cache/packages'
        $package = Get-ChildItem -LiteralPath $packages -Directory -Filter 'GameExport-*' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $package) { throw 'flow package not found' }
        $launch = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $package.FullName 'Start-Game.ps1'),'-Headless','-Frames','2','-Locale','en-US') -PassThru -Wait -RedirectStandardOutput (Join-Path $OutputDirectory 'package-launch.log') -RedirectStandardError (Join-Path $OutputDirectory 'package-launch.err.log')
        if ($launch.ExitCode -ne 0) { throw "flow package launch exit $($launch.ExitCode)" }
    }
    elseif ($Cancel) {
        if ($afterStart -notmatch 'EditorUi\.ToolkitExport: finished cancelled') { throw 'auto export was not cancelled' }
    } else {
        if ($afterStart -notmatch 'EditorUi\.ToolkitExport: finished succeeded') { throw 'auto export did not succeed' }
        $packages = Join-Path $repo '.cache/packages'
        $package = Get-ChildItem -LiteralPath $packages -Directory -Filter 'GameExport-*' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $package) { throw 'export package not found' }
        $launch = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $package.FullName 'Start-Game.ps1'),'-Headless','-Frames','2','-Locale','en-US') -PassThru -Wait -RedirectStandardOutput (Join-Path $OutputDirectory 'package-launch.log') -RedirectStandardError (Join-Path $OutputDirectory 'package-launch.err.log')
        if ($launch.ExitCode -ne 0) { throw "exported package launch exit $($launch.ExitCode)" }
    }
    @{ success=$true; exportStarted=$true; exportSucceeded=(-not $Cancel); exportCancelled=$Cancel.IsPresent; packageLaunch=(-not $Cancel) } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
    Write-Output 'EditorToolkitExportHeadless passed.'
}
finally {
    Remove-Item Env:SELFENGINE_EDITOR_AUTO_EXPORT -ErrorAction SilentlyContinue
    Remove-Item Env:SELFENGINE_EDITOR_OPEN_PANEL -ErrorAction SilentlyContinue
    Remove-Item Env:SELFENGINE_EDITOR_AUTO_EXPORT_CANCEL_MS -ErrorAction SilentlyContinue
    Remove-Item Env:SELFENGINE_EDITOR_AUTO_EXPORT_FLOW -ErrorAction SilentlyContinue
    if (-not $process.HasExited) { cmd /c "taskkill /PID $($process.Id) /T /F >nul 2>&1" | Out-Null }
}
