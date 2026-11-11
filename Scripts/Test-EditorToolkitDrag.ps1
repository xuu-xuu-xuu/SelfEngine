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
public static class ToolkitDragWin32 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static List<IntPtr> ForProcess(uint pidTarget){var r=new List<IntPtr>();EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pidTarget)r.Add(h);return true;},IntPtr.Zero);return r;}
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
foreach ($window in [ToolkitDragWin32]::ForProcess([uint32]$process.Id)) {
    if ([ToolkitDragWin32]::IsWindowVisible($window) -and [ToolkitDragWin32]::Title($window) -eq '') { $target = $window; break }
}
if ($target -eq [IntPtr]::Zero) { if (-not $process.HasExited) { $process.Kill() }; throw 'engine window not found' }
function Post([uint32]$msg,[int]$x,[int]$y,[int]$buttons=0){ [ToolkitDragWin32]::PostMessage($target,$msg,[IntPtr]$buttons,[IntPtr]($x -bor ($y -shl 16))) | Out-Null }
Post 0x0201 30 108 1
Start-Sleep -Milliseconds 100
Post 0x0200 30 96 1
Post 0x0200 30 84 1
Start-Sleep -Milliseconds 100
Post 0x0202 30 84 0
Start-Sleep -Seconds 1
Post 0x0010 0 0 0
Start-Sleep -Seconds 1
# Dirty close protection: F10 discards in the modal (keyboard fallback).
Post 0x0100 0 0 0x79
Post 0x0101 0 0 0x79
if (-not $process.WaitForExit(15000)) { $process.Kill(); throw 'drag smoke did not exit' }
$log = Get-Content -LiteralPath $stdout -Raw
if ($log -notmatch 'EditorUi\.ToolkitReparent:') { throw 'reparent was not observed' }
if ($log -notmatch 'EditorUi\.ToolkitPersisted') { throw 'persist was not observed' }
@{ success = $true; reparent = $true; normalClose = $true } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
Write-Output 'EditorToolkitDrag passed (real hierarchy drag + WM_CLOSE).'
