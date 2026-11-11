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
public static class ToolkitMaterialWin32 {
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
$material = @'
{
  "type": "SelfEngine.Material",
  "version": 1,
  "name": "TestMat",
  "baseColor": [1.0, 0.0, 0.0, 1.0],
  "metallic": 0.0,
  "roughness": 0.2,
  "normalScale": 1.0,
  "doubleSided": false,
  "shader": "",
  "textures": { "baseColor": "", "normal": "", "metallicRoughness": "" }
}
'@
Set-Content -Path (Join-Path $project 'Content/Test.material.json') -Value $material -Encoding utf8
@'
{
  "version": 1,
  "id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "options": { "scale": 1.0, "generateTangents": false }
}
'@ | Set-Content -Path (Join-Path $project 'Content/Test.material.json.asset.json') -Encoding utf8

$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
$process = Start-Process -FilePath $Executable -ArgumentList @('--worker','--project',$project,'--editor-ui=toolkit') -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
function Click-Client([int]$x,[int]$y) {
    $point = [IntPtr]($x -bor ($y -shl 16))
    [ToolkitMaterialWin32]::PostMessage($target,0x0201,[IntPtr]1,$point) | Out-Null
    Start-Sleep -Milliseconds 80
    [ToolkitMaterialWin32]::PostMessage($target,0x0202,[IntPtr]0,$point) | Out-Null
    Start-Sleep -Milliseconds 200
}
try {
    Start-Sleep -Seconds 6
    $target = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 24 -and $target -eq [IntPtr]::Zero; ++$attempt) {
        foreach ($window in [ToolkitMaterialWin32]::ForProcess([uint32]$process.Id)) {
            if ([ToolkitMaterialWin32]::IsWindowVisible($window) -and [ToolkitMaterialWin32]::Title($window) -eq '') { $target = $window; break }
        }
        if ($target -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
    }
    if ($target -eq [IntPtr]::Zero) { throw 'engine window not found' }

    # Assets group is the lower-left dock; scan the first few rows because the
    # exact row height depends on the DPI-derived logical size.
    foreach ($y in @(548, 560, 572, 584, 596)) {
        Click-Client 30 $y
        Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Seconds 2
    # Material form row 2 (metallic) in the lower-right group.
    Click-Client 1000 638
    Start-Sleep -Milliseconds 300
    foreach ($ch in '0.5'.ToCharArray()) {
        [ToolkitMaterialWin32]::PostMessage($target,0x0102,[IntPtr][char]$ch,[IntPtr]0) | Out-Null
    }
    Start-Sleep -Milliseconds 100
    [ToolkitMaterialWin32]::PostMessage($target,0x0100,[IntPtr]13,[IntPtr]0) | Out-Null
    [ToolkitMaterialWin32]::PostMessage($target,0x0101,[IntPtr]13,[IntPtr]0) | Out-Null
    Start-Sleep -Milliseconds 300
    # Save button at the Material panel top-left.
    Click-Client 1000 566
    Start-Sleep -Seconds 2
    [ToolkitMaterialWin32]::PostMessage($target,0x0010,[IntPtr]0,[IntPtr]0) | Out-Null
    if (-not $process.WaitForExit(15000)) { throw 'editor did not exit' }
    $log = Get-Content -LiteralPath $stdout -Raw
    if ($log -notmatch 'EditorUi\.ToolkitMaterial: opened') { throw 'material was not opened' }
    if ($log -notmatch 'EditorUi\.ToolkitMaterial: property material\.metallic') { throw 'material property not committed' }
    if ($log -notmatch 'EditorUi\.ToolkitMaterial: saved') { throw 'material not saved' }
    $saved = Get-Content -LiteralPath (Join-Path $project 'Content/Test.material.json') -Raw
    if ($saved -notmatch '"metallic"\s*:\s*0\.5') { throw 'material file does not contain 0.5' }
    @{ success=$true; opened=$true; property=$true; saved=$true; fileChanged=$true } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding utf8
    Write-Output 'EditorToolkitMaterial passed (open, property commit, save).'
}
finally {
    if (-not $process.HasExited) {
        cmd /c "taskkill /PID $($process.Id) /T /F >nul 2>&1" | Out-Null
    }
}
