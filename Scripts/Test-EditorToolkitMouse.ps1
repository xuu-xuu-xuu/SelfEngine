param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Continue'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ToolkitMouseWin32 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static List<IntPtr> ForProcess(uint target) {
        var result = new List<IntPtr>();
        EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid); if (pid == target) result.Add(h); return true; }, IntPtr.Zero);
        return result;
    }
    public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
}
'@

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$repo = Split-Path -Parent $PSScriptRoot
$template = Join-Path $repo 'Resources/Templates/BlankScene'
$project = Join-Path $OutputDirectory 'project'
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
if (Test-Path -LiteralPath $project) { Remove-Item -Recurse -Force $project }
Copy-Item -Recurse $template $project

$process = Start-Process -FilePath $Executable -ArgumentList @('--worker', '--project', $project, '--editor-ui=toolkit') -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
Start-Sleep -Seconds 5
$windows = [ToolkitMouseWin32]::ForProcess([uint32]$process.Id)
$target = [IntPtr]::Zero
foreach ($window in $windows) {
    if ([ToolkitMouseWin32]::IsWindowVisible($window) -and [ToolkitMouseWin32]::Title($window) -eq '') {
        $target = $window
        break
    }
}
if ($target -eq [IntPtr]::Zero) {
    if (-not $process.HasExited) { $process.Kill() }
    throw 'toolkit engine window not found'
}

$point = [IntPtr](30 -bor (90 -shl 16))
[ToolkitMouseWin32]::PostMessage($target, 0x0201, [IntPtr]1, $point) | Out-Null
Start-Sleep -Milliseconds 100
[ToolkitMouseWin32]::PostMessage($target, 0x0202, [IntPtr]::Zero, $point) | Out-Null
Start-Sleep -Seconds 1
$viewportPoint = [IntPtr](614 -bor (408 -shl 16))
[ToolkitMouseWin32]::PostMessage($target, 0x0200, [IntPtr]0, $viewportPoint) | Out-Null
Start-Sleep -Milliseconds 100
[ToolkitMouseWin32]::PostMessage($target, 0x0201, [IntPtr]1, $viewportPoint) | Out-Null
Start-Sleep -Milliseconds 100
[ToolkitMouseWin32]::PostMessage($target, 0x0202, [IntPtr]::Zero, $viewportPoint) | Out-Null
Start-Sleep -Seconds 1
[ToolkitMouseWin32]::PostMessage($target, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
if (-not $process.WaitForExit(15000)) {
    $process.Kill()
    throw 'toolkit mouse smoke did not exit'
}

$log = Get-Content -LiteralPath $stdout -Raw
if ($log -notmatch 'EditorUi\.ToolkitSelection:') { throw 'toolkit selection was not observed' }
if ($log -notmatch 'EditorUi\.ToolkitPick:') { throw 'toolkit viewport pick was not observed' }
if ($log -notmatch 'EditorUi\.ToolkitPersisted') { throw 'toolkit did not persist after close' }

@{ success = $true; selection = $true; normalClose = $true } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
Write-Output 'EditorToolkitMouse passed (real hierarchy click + WM_CLOSE).'
