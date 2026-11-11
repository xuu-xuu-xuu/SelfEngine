param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ToolkitDesignerWin32 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
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
Copy-Item -Recurse (Join-Path $repo 'build-g/Resources/Templates/EmptyGame') $project
$uiFile = Join-Path $project 'UI/Main.ui.json'
$before = (Get-FileHash -LiteralPath $uiFile -Algorithm SHA256).Hash
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
$env:SELFENGINE_EDITOR_OPEN_PANEL = 'ui-designer'
$process = Start-Process -FilePath $Executable -ArgumentList @('--worker','--project',$project,'--editor-ui=toolkit') -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
function Click-Client([int]$x,[int]$y,[int]$hold=80) {
    $point = [IntPtr]($x -bor ($y -shl 16))
    [ToolkitDesignerWin32]::PostMessage($target,0x0201,[IntPtr]1,$point) | Out-Null
    Start-Sleep -Milliseconds $hold
    [ToolkitDesignerWin32]::PostMessage($target,0x0202,[IntPtr]0,$point) | Out-Null
    Start-Sleep -Milliseconds 250
}
try {
    Start-Sleep -Seconds 6
    $target = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 24 -and $target -eq [IntPtr]::Zero; ++$attempt) {
        foreach ($window in [ToolkitDesignerWin32]::ForProcess([uint32]$process.Id)) {
            if ([ToolkitDesignerWin32]::IsWindowVisible($window) -and [ToolkitDesignerWin32]::Title($window) -eq '') { $target = $window; break }
        }
        if ($target -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
    }
    if ($target -eq [IntPtr]::Zero) { throw 'engine window not found' }

    # The test hook opens UI Designer at startup (stable across localized tab widths).
    Start-Sleep -Seconds 1
    # Select and drag node 1 (inside the designer canvas, outside node 2).
    $start = [IntPtr](330 -bor (250 -shl 16))
    [ToolkitDesignerWin32]::PostMessage($target,0x0201,[IntPtr]1,$start) | Out-Null
    Start-Sleep -Milliseconds 100
    $move = [IntPtr](380 -bor (280 -shl 16))
    [ToolkitDesignerWin32]::PostMessage($target,0x0200,[IntPtr]1,$move) | Out-Null
    Start-Sleep -Milliseconds 100
    [ToolkitDesignerWin32]::PostMessage($target,0x0202,[IntPtr]0,$move) | Out-Null
    Start-Sleep -Seconds 2

    $rect = New-Object ToolkitDesignerWin32+RECT
    [ToolkitDesignerWin32]::GetWindowRect($target, [ref]$rect) | Out-Null
    $bitmap = New-Object System.Drawing.Bitmap ($rect.Right-$rect.Left), ($rect.Bottom-$rect.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    $bitmap.Save((Join-Path $OutputDirectory 'designer.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()

    [ToolkitDesignerWin32]::PostMessage($target,0x0010,[IntPtr]0,[IntPtr]0) | Out-Null
    if (-not $process.WaitForExit(15000)) { throw 'editor did not exit' }
    $log = Get-Content -LiteralPath $stdout -Raw
    if ($log -notmatch 'EditorUi\.ToolkitUi: loaded') { throw 'UI document was not loaded' }
    if ($log -notmatch 'EditorUi\.ToolkitUi: saved') { throw 'UI drag was not saved' }
    $after = (Get-FileHash -LiteralPath $uiFile -Algorithm SHA256).Hash
    if ($after -eq $before) { throw 'UI file did not change' }
    @{ success=$true; loaded=$true; dragged=$true; saved=$true } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
    Write-Output 'EditorToolkitDesigner passed (load, drag, save).'
}
finally {
    Remove-Item Env:SELFENGINE_EDITOR_OPEN_PANEL -ErrorAction SilentlyContinue
    if (-not $process.HasExited) {
        cmd /c "taskkill /PID $($process.Id) /T /F >nul 2>&1" | Out-Null
    }
}
