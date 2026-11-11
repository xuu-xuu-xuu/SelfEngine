function Invoke-EngineProgram([string]$Executable,[string[]]$Arguments,[string]$LogPath,[switch]$VisibleWindow) {
    $info=New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName=(Resolve-Path -LiteralPath $Executable).Path
    if([IO.Path]::GetFileName($info.FileName) -ieq 'powershell.exe'){
        # 仅为 Windows PowerShell 子进程优先提供其系统模块；保留自定义搜索路径。
        $systemModules=Join-Path (Split-Path -Parent $info.FileName) 'Modules'
        $info.EnvironmentVariables['PSModulePath']=$systemModules+';'+$env:PSModulePath
    }
    $quoted=@(foreach($argument in $Arguments){
        $value=[regex]::Replace($argument,'(\\*)"','$1$1\"')
        $value=[regex]::Replace($value,'(\\+)$','$1$1')
        '"'+$value+'"'
    })
    $info.Arguments=$quoted -join ' '
    $info.WorkingDirectory=(Get-Location).Path
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.WindowStyle=if($VisibleWindow){[Diagnostics.ProcessWindowStyle]::Normal}else{[Diagnostics.ProcessWindowStyle]::Hidden}
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $info.StandardOutputEncoding=[Text.Encoding]::UTF8;$info.StandardErrorEncoding=[Text.Encoding]::UTF8
    $process=New-Object System.Diagnostics.Process
    $process.StartInfo=$info
    try {
        [void]$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $encoding=New-Object Text.UTF8Encoding($false)
        [IO.File]::WriteAllText([IO.Path]::GetFullPath($LogPath),$stdout.Result+$stderr.Result,$encoding)
        return $process.ExitCode
    } finally {$process.Dispose()}
}
if($PSVersionTable.PSVersion.Major -le 5){
    # 本文件也被由 .NET 直接启动的预览入口载入，必须在首次 Get-FileHash 前选择兼容模块。
    Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
}
