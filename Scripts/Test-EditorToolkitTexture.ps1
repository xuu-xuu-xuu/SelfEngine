param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ToolkitTextureWin32 {
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
Copy-Item -Recurse (Join-Path $repo 'Resources/Templates/BlankScene') $project
$textureSource = Join-Path $repo 'Resources/Templates/EmptyGame/Content/Models'
$textureTarget = Join-Path $project 'Content/Models'
New-Item -ItemType Directory -Force -Path $textureTarget | Out-Null
Copy-Item -LiteralPath (Join-Path $textureSource 'checker.png') -Destination $textureTarget
Copy-Item -LiteralPath (Join-Path $textureSource 'checker.png.asset.json') -Destination $textureTarget
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
$env:SELFENGINE_EDITOR_OPEN_ASSET = 'checker.png'
$process = Start-Process -FilePath $Executable -ArgumentList @('--worker','--project',$project,'--editor-ui=toolkit') -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
function Click-Client([IntPtr]$target,[int]$x,[int]$y) {
    $point = [IntPtr]($x -bor ($y -shl 16))
    [ToolkitTextureWin32]::PostMessage($target,0x0201,[IntPtr]1,$point) | Out-Null
    Start-Sleep -Milliseconds 80
    [ToolkitTextureWin32]::PostMessage($target,0x0202,[IntPtr]0,$point) | Out-Null
    Start-Sleep -Milliseconds 250
}
try {
    Start-Sleep -Seconds 6
    $target = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 30 -and $target -eq [IntPtr]::Zero; ++$attempt) {
        foreach ($window in [ToolkitTextureWin32]::ForProcess([uint32]$process.Id)) {
            if ([ToolkitTextureWin32]::IsWindowVisible($window) -and [ToolkitTextureWin32]::Title($window) -eq '') { $target = $window; break }
        }
        if ($target -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
    }
    if ($target -eq [IntPtr]::Zero) { throw 'engine window not found' }
    [ToolkitTextureWin32]::ShowWindow($target,5) | Out-Null
    [ToolkitTextureWin32]::SetForegroundWindow($target) | Out-Null
    for ($wait = 0; $wait -lt 30; ++$wait) {
        if ((Test-Path -LiteralPath $stdout) -and ((Get-Content -LiteralPath $stdout -Raw) -match 'EditorUi\.ToolkitExtract')) { break }
        Start-Sleep -Milliseconds 500
    }
    for ($wait = 0; $wait -lt 60; ++$wait) {
        if ((Test-Path -LiteralPath $stdout) -and ((Get-Content -LiteralPath $stdout -Raw) -match 'EditorUi\.ToolkitAssetPreview')) { break }
        Start-Sleep -Seconds 1
    }
    $log = Get-Content -LiteralPath $stdout -Raw
    if ($log -notmatch 'EditorUi\.ToolkitAssetPreview') { throw 'texture preview was not opened' }
    if ($log -match 'EditorUi\.ToolkitGeometryFailed') { throw 'texture preview failed geometry' }
    Add-Type -AssemblyName System.Drawing
    $rect = [ToolkitTextureWin32+RECT]::new()
    if ([ToolkitTextureWin32]::GetWindowRect($target,[ref]$rect) -and $rect.Left -gt -10000) {
        $w = $rect.Right - $rect.Left
        $h = $rect.Bottom - $rect.Top
        $bmp = New-Object System.Drawing.Bitmap($w,$h)
        $graphics = [System.Drawing.Graphics]::FromImage($bmp)
        $graphics.CopyFromScreen($rect.Left,$rect.Top,0,0,[System.Drawing.Size]::new($w,$h))
        $bmp.Save((Join-Path $OutputDirectory 'texture.png'),[System.Drawing.Imaging.ImageFormat]::Png)
        $graphics.Dispose(); $bmp.Dispose()
    }
    [ToolkitTextureWin32]::PostMessage($target,0x0010,[IntPtr]0,[IntPtr]0) | Out-Null
    if (-not $process.WaitForExit(15000)) { throw 'editor did not exit' }
    @{ success=$true; texturePreview=$true; log=$stdout } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
    Write-Output 'EditorToolkitTexture passed (texture thumbnail drew without geometry failure).'
}
finally {
    Remove-Item Env:SELFENGINE_EDITOR_OPEN_ASSET -ErrorAction SilentlyContinue
    if (-not $process.HasExited) { cmd /c "taskkill /PID $($process.Id) /T /F >nul 2>&1" | Out-Null }
}
