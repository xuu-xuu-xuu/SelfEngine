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
public static class ToolkitInspectorWin32 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
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
$windows = [ToolkitInspectorWin32]::ForProcess([uint32]$process.Id)
$target = [IntPtr]::Zero
foreach ($window in $windows) {
    if ([ToolkitInspectorWin32]::IsWindowVisible($window) -and [ToolkitInspectorWin32]::Title($window) -eq '') {
        $target = $window
        break
    }
}
if ($target -eq [IntPtr]::Zero) {
    if (-not $process.HasExited) { $process.Kill() }
    throw 'toolkit engine window not found'
}

function Click-Client([int]$x, [int]$y) {
    $point = [IntPtr]($x -bor ($y -shl 16))
    [ToolkitInspectorWin32]::PostMessage($target, 0x0201, [IntPtr]1, $point) | Out-Null
    Start-Sleep -Milliseconds 80
    [ToolkitInspectorWin32]::PostMessage($target, 0x0202, [IntPtr]::Zero, $point) | Out-Null
}

Click-Client 30 90
Start-Sleep -Seconds 1
Click-Client 1000 90
Start-Sleep -Milliseconds 300
[ToolkitInspectorWin32]::PostMessage($target, 0x0102, [IntPtr][char]'X', [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 100
[ToolkitInspectorWin32]::PostMessage($target, 0x0100, [IntPtr]13, [IntPtr]::Zero) | Out-Null
[ToolkitInspectorWin32]::PostMessage($target, 0x0101, [IntPtr]13, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 1
[ToolkitInspectorWin32]::PostMessage($target, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 1
Add-Type -AssemblyName System.Drawing
$rect = New-Object ToolkitInspectorWin32+RECT
[ToolkitInspectorWin32]::GetWindowRect($target, [ref]$rect) | Out-Null
$bitmap = New-Object System.Drawing.Bitmap ($rect.Right-$rect.Left), ($rect.Bottom-$rect.Top)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
$bitmap.Save((Join-Path $OutputDirectory 'close-modal.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $bitmap.Dispose()
# Dirty close protection: F10 discards in the modal (keyboard fallback).
[ToolkitInspectorWin32]::PostMessage($target, 0x0100, [IntPtr]0x79, [IntPtr]::Zero) | Out-Null
[ToolkitInspectorWin32]::PostMessage($target, 0x0101, [IntPtr]0x79, [IntPtr]::Zero) | Out-Null
if (-not $process.WaitForExit(15000)) {
    $process.Kill()
    throw 'toolkit inspector smoke did not exit'
}

$log = Get-Content -LiteralPath $stdout -Raw
if ($log -notmatch 'EditorUi\.ToolkitProperty: name') { throw 'toolkit inspector property commit was not observed' }
if ($log -notmatch 'EditorUi\.ToolkitPersisted') { throw 'toolkit did not persist after close' }

@{ success = $true; propertyCommit = $true; normalClose = $true } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
Write-Output 'EditorToolkitInspector passed (real property edit + Enter + WM_CLOSE).'
