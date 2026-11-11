param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$TimeoutSeconds = 240,
    [switch]$Cancel,
    [switch]$Flow
)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ToolkitExportWin32 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
    public static List<IntPtr> ForProcess(uint target){
        var r=new List<IntPtr>();
        EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==target)r.Add(h);return true;},IntPtr.Zero);
        return r;
    }
    public static string Title(IntPtr h){var s=new StringBuilder(256);GetWindowText(h,s,256);return s.ToString();}
}
'@
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $OutputDirectory 'project'
if (Test-Path -LiteralPath $project) { Remove-Item -Recurse -Force $project }
Copy-Item -Recurse (Join-Path $repo 'Resources/Templates/EmptyGame') $project
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
$env:SELFENGINE_EDITOR_OPEN_PANEL = 'build-export'
$process = Start-Process -FilePath $Executable -ArgumentList @('--worker','--project',$project,'--editor-ui=toolkit') -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try {
    $target = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 30 -and $target -eq [IntPtr]::Zero; ++$attempt) {
        Start-Sleep -Milliseconds 500
        $bestArea = 0
        foreach ($window in [ToolkitExportWin32]::ForProcess([uint32]$process.Id)) {
            if (-not [ToolkitExportWin32]::IsWindowVisible($window) -or [ToolkitExportWin32]::Title($window) -ne '') { continue }
            $rect = [ToolkitExportWin32+RECT]::new()
            if ([ToolkitExportWin32]::GetWindowRect($window, [ref]$rect)) {
                $area = [int64]($rect.Right - $rect.Left) * [int64]($rect.Bottom - $rect.Top)
                if ($area -gt $bestArea) { $bestArea = $area; $target = $window }
            }
        }
    }
    if ($target -eq [IntPtr]::Zero) { throw 'engine window not found' }
    [ToolkitExportWin32]::ShowWindow($target,5) | Out-Null
    [ToolkitExportWin32]::SetForegroundWindow($target) | Out-Null
    for ($wait = 0; $wait -lt 30; ++$wait) {
        if ((Test-Path -LiteralPath $stdout) -and ((Get-Content -LiteralPath $stdout -Raw) -match 'EditorUi\.ToolkitExtract')) { break }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Seconds 2
    $pointSave = [IntPtr]((1000) -bor (566 -shl 16))
    [ToolkitExportWin32]::PostMessage($target,0x0201,[IntPtr]1,$pointSave) | Out-Null
    Start-Sleep -Milliseconds 80
    [ToolkitExportWin32]::PostMessage($target,0x0202,[IntPtr]0,$pointSave) | Out-Null
    Start-Sleep -Seconds 1
    $pointExport = [IntPtr]((1090) -bor (566 -shl 16))
    [ToolkitExportWin32]::PostMessage($target,0x0201,[IntPtr]1,$pointExport) | Out-Null
    Start-Sleep -Milliseconds 80
    [ToolkitExportWin32]::PostMessage($target,0x0202,[IntPtr]0,$pointExport) | Out-Null
    if ($Flow) {
        Start-Sleep -Seconds 2
        # 第一次点击 Cancel，等待 cancelled。
        [ToolkitExportWin32]::PostMessage($target,0x0201,[IntPtr]1,$pointExport) | Out-Null
        Start-Sleep -Milliseconds 80
        [ToolkitExportWin32]::PostMessage($target,0x0202,[IntPtr]0,$pointExport) | Out-Null
        $cancelled = $false
        for ($wait = 0; $wait -lt 120; ++$wait) {
            if ((Get-Content -LiteralPath $stdout -Raw) -match 'EditorUi\.ToolkitExport: finished cancelled') { $cancelled = $true; break }
            Start-Sleep -Seconds 1
        }
        if (-not $cancelled) { throw 'flow cancel was not observed' }
        # 同一个进程内再导出一次，验证成功后退出。
        Start-Sleep -Seconds 2
        [ToolkitExportWin32]::PostMessage($target,0x0201,[IntPtr]1,$pointExport) | Out-Null
        Start-Sleep -Milliseconds 80
        [ToolkitExportWin32]::PostMessage($target,0x0202,[IntPtr]0,$pointExport) | Out-Null
        $succeeded = $false
        for ($wait = 0; $wait -lt 240; ++$wait) {
            if ((Get-Content -LiteralPath $stdout -Raw) -match 'EditorUi\.ToolkitExport: finished succeeded') { $succeeded = $true; break }
            Start-Sleep -Seconds 1
        }
        if (-not $succeeded) { throw 'flow success was not observed' }
        [ToolkitExportWin32]::PostMessage($target,0x0010,[IntPtr]0,[IntPtr]0) | Out-Null
        if (-not $process.WaitForExit(30000)) { throw 'editor did not exit after flow' }
        $package = Get-ChildItem -LiteralPath (Join-Path $repo '.cache/packages') -Directory -Filter 'GameExport-*' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $package) { throw 'flow package not found' }
        $launch = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $package.FullName 'Start-Game.ps1'),'-Headless','-Frames','2','-Locale','en-US') -PassThru -Wait -RedirectStandardOutput (Join-Path $OutputDirectory 'package-launch.log') -RedirectStandardError (Join-Path $OutputDirectory 'package-launch.err.log')
        if ($launch.ExitCode -ne 0) { throw "flow package launch exit $($launch.ExitCode)" }
        @{ success=$true; exportStarted=$true; exportCancelled=$true; exportSucceeded=$true; packageLaunch=$true; package=$package.FullName; log=(Join-Path $OutputDirectory 'stdout.log') } | ConvertTo-Json |
            Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
        Write-Output 'EditorToolkitExportFlow passed (cancel then success in one process).'
        return
    }
    if ($Cancel) {
        Start-Sleep -Seconds 2
        [ToolkitExportWin32]::PostMessage($target,0x0201,[IntPtr]1,$pointExport) | Out-Null
        Start-Sleep -Milliseconds 80
        [ToolkitExportWin32]::PostMessage($target,0x0202,[IntPtr]0,$pointExport) | Out-Null
    }
    $finished = $false
    $afterStart = ''
    for ($wait = 0; $wait -lt $TimeoutSeconds; ++$wait) {
        $log = Get-Content -LiteralPath $stdout -Raw
        $startAt = $log.LastIndexOf('EditorUi.ToolkitExport: start')
        if ($startAt -ge 0) {
            $afterStart = $log.Substring($startAt)
            if ($afterStart -match 'EditorUi\.ToolkitExport: finished') { $finished = $true; break }
        }
        Start-Sleep -Seconds 1
    }
    if (-not $finished) { throw "export did not finish in $TimeoutSeconds s" }
    if ($Cancel) {
        if ($afterStart -notmatch 'EditorUi\.ToolkitExport: finished cancelled') { throw 'export was not cancelled' }
    } else {
        if ($afterStart -notmatch 'EditorUi\.ToolkitExport: finished succeeded') { throw 'export did not succeed' }
    }
    [ToolkitExportWin32]::PostMessage($target,0x0010,[IntPtr]0,[IntPtr]0) | Out-Null
    if (-not $process.WaitForExit(30000)) { throw 'editor did not exit after export' }
    $packageLaunch = $false
    $packagePath = ''
    if (-not $Cancel) {
        $packages = Join-Path $repo '.cache/packages'
        $package = Get-ChildItem -LiteralPath $packages -Directory -Filter 'GameExport-*' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $package) { throw 'export package not found' }
        $packagePath = $package.FullName
        $launchLog = Join-Path $OutputDirectory 'package-launch.log'
        $launch = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $package.FullName 'Start-Game.ps1'),'-Headless','-Frames','2','-Locale','en-US') -PassThru -Wait -RedirectStandardOutput $launchLog -RedirectStandardError (Join-Path $OutputDirectory 'package-launch.err.log')
        if ($launch.ExitCode -ne 0) { throw "exported package launch exit $($launch.ExitCode)" }
        $packageLaunch = $true
    }
    @{ success=$true; exportStarted=$true; exportSucceeded=(-not $Cancel); exportCancelled=$Cancel.IsPresent; package=$packagePath; packageLaunch=$packageLaunch; log=(Join-Path $OutputDirectory 'stdout.log') } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
    if ($Cancel) { Write-Output 'EditorToolkitExport passed (real export cancelled and cleaned up).' }
    else { Write-Output 'EditorToolkitExport passed (real EmptyGame export succeeded).' }
}
finally {
    Remove-Item Env:SELFENGINE_EDITOR_OPEN_PANEL -ErrorAction SilentlyContinue
    if (-not $process.HasExited) {
        cmd /c "taskkill /PID $($process.Id) /T /F >nul 2>&1" | Out-Null
    }
}
