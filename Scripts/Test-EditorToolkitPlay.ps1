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
public static class ToolkitPlayWin32 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public static List<IntPtr> ForProcess(uint target){var r=new List<IntPtr>();EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==target)r.Add(h);return true;},IntPtr.Zero);return r;}
  public static string Title(IntPtr h){var s=new StringBuilder(256);GetWindowText(h,s,256);return s.ToString();}
}
'@
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $OutputDirectory 'project'
if (Test-Path -LiteralPath $project) { Remove-Item -Recurse -Force $project }
Copy-Item -Recurse (Join-Path $repo 'Resources/Templates/BlankScene') $project
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
$process = Start-Process -FilePath $Executable -ArgumentList @('--worker','--project',$project,'--editor-ui=toolkit') -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
Start-Sleep -Seconds 5
$target = [IntPtr]::Zero
$bestArea = 0
foreach ($window in [ToolkitPlayWin32]::ForProcess([uint32]$process.Id)) {
    if (-not [ToolkitPlayWin32]::IsWindowVisible($window) -or [ToolkitPlayWin32]::Title($window) -ne '') { continue }
    $rect = [ToolkitPlayWin32+RECT]::new()
    if ([ToolkitPlayWin32]::GetWindowRect($window,[ref]$rect)) {
        $area = [int64]($rect.Right-$rect.Left)*[int64]($rect.Bottom-$rect.Top)
        if ($area -gt $bestArea) { $bestArea = $area; $target = $window }
    }
}
if ($target -eq [IntPtr]::Zero) { if (-not $process.HasExited) { $process.Kill() }; throw 'engine window not found' }
[ToolkitPlayWin32]::PostMessage($target,0x0100,[IntPtr]0x74,[IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 500
[ToolkitPlayWin32]::PostMessage($target,0x0100,[IntPtr]0x57,[IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
[ToolkitPlayWin32]::PostMessage($target,0x0101,[IntPtr]0x57,[IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 1
[ToolkitPlayWin32]::PostMessage($target,0x0100,[IntPtr]0x74,[IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 1
for ($wait = 0; $wait -lt 20; ++$wait) {
    if ((Get-Content -LiteralPath $stdout -Raw) -match 'ToolkitPlay: stop') { break }
    Start-Sleep -Milliseconds 200
}
if ((Get-Content -LiteralPath $stdout -Raw) -notmatch 'ToolkitPlay: stop') {
    [ToolkitPlayWin32]::PostMessage($target,0x0100,[IntPtr]0x77,[IntPtr]::Zero) | Out-Null
    [ToolkitPlayWin32]::PostMessage($target,0x0101,[IntPtr]0x77,[IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 1
}
[ToolkitPlayWin32]::PostMessage($target,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
if (-not $process.WaitForExit(15000)) { $process.Kill(); throw 'play smoke did not exit' }
$log = Get-Content -LiteralPath $stdout -Raw
if ($log -notmatch 'ToolkitPlay: start') { throw 'play start not observed' }
if ($log -notmatch 'ToolkitPlay: stop') { throw 'play stop not observed' }
if ($log -notmatch 'EditorUi.ToolkitViewport:') { throw 'toolkit viewport rect not observed' }
if ($log -notmatch 'EditorUi.ToolkitGameplayInput') { throw 'toolkit gameplay input not observed' }
@{ success = $true; playStart = $true; playStop = $true } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
Write-Output 'EditorToolkitPlay passed (F5 start/stop).'
