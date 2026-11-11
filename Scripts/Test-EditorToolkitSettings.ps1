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
public static class ToolkitSettingsWin32 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
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
$stdout = Join-Path $OutputDirectory 'stdout.log'

function Start-Editor([string]$panel) {
    $env:SELFENGINE_EDITOR_OPEN_PANEL = $panel
    $outFile = Join-Path $OutputDirectory "stdout-$panel.log"
    $process = Start-Process -FilePath $Executable -ArgumentList @('--worker','--project',$project,'--editor-ui=toolkit') -PassThru -RedirectStandardOutput $outFile -RedirectStandardError (Join-Path $OutputDirectory "stderr-$panel.log")
    $target = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 24 -and $target -eq [IntPtr]::Zero; ++$attempt) {
        Start-Sleep -Milliseconds 500
        foreach ($window in [ToolkitSettingsWin32]::ForProcess([uint32]$process.Id)) {
            if ([ToolkitSettingsWin32]::IsWindowVisible($window) -and [ToolkitSettingsWin32]::Title($window) -eq '') { $target = $window; break }
        }
    }
    if ($target -eq [IntPtr]::Zero) { throw "engine window not found for $panel" }
    for ($wait = 0; $wait -lt 30; ++$wait) {
        if ((Test-Path -LiteralPath $outFile) -and ((Get-Content -LiteralPath $outFile -Raw) -match 'EditorUi\.ToolkitExtract')) { break }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Seconds 2
    return @{ Process = $process; Target = $target; OutFile = $outFile }
}

function Click-Client([IntPtr]$target,[int]$x,[int]$y) {
    $point = [IntPtr]($x -bor ($y -shl 16))
    [ToolkitSettingsWin32]::PostMessage($target,0x0201,[IntPtr]1,$point) | Out-Null
    Start-Sleep -Milliseconds 80
    [ToolkitSettingsWin32]::PostMessage($target,0x0202,[IntPtr]0,$point) | Out-Null
    Start-Sleep -Milliseconds 300
}

function Close-Editor($session) {
    [ToolkitSettingsWin32]::PostMessage($session.Target,0x0010,[IntPtr]0,[IntPtr]0) | Out-Null
    if (-not $session.Process.WaitForExit(15000)) {
        cmd /c "taskkill /PID $($session.Process.Id) /T /F >nul 2>&1" | Out-Null
        throw 'editor did not exit'
    }
}

try {
    $settings = Start-Editor 'project-settings'
    Click-Client $settings.Target 1000 566
    Start-Sleep -Seconds 1
    Close-Editor $settings

    $build = Start-Editor 'build-export'
    Click-Client $build.Target 1000 566
    Start-Sleep -Seconds 1
    Click-Client $build.Target 1090 566
    Start-Sleep -Seconds 1
    Click-Client $build.Target 1090 566
    Start-Sleep -Seconds 2
    Close-Editor $build

    $projectLog = Get-Content -LiteralPath $settings.OutFile -Raw
    $buildLog = Get-Content -LiteralPath $build.OutFile -Raw
    if ($projectLog -notmatch 'EditorUi\.ToolkitProjectSettings: saved') { throw 'project settings not saved' }
    if ($buildLog -notmatch 'EditorUi\.ToolkitBuildSettings: saved') { throw 'build settings not saved' }
    if ($buildLog -notmatch 'EditorUi\.ToolkitExport: start') { throw 'export action was not started' }
    if (-not (Test-Path -LiteralPath (Join-Path $project 'build.json'))) { throw 'build.json was not created' }
    @{ success=$true; projectPanel=$true; buildPanel=$true; projectSaved=$true; buildSaved=$true; exportStarted=$true } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
    Write-Output 'EditorToolkitSettings passed (Project Settings + Build/Export save).'
}
finally {
    Remove-Item Env:SELFENGINE_EDITOR_OPEN_PANEL -ErrorAction SilentlyContinue
}
