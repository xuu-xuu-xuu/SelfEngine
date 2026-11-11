# VS 的默认 PowerShell 入口可能按控制台代码页解码环境，破坏中文目录。
# 子进程只输出 UTF-16 字符串的 Base64，不修改调用者的控制台编码。
function Import-EngineMsvcEnvironment {
    param([string]$VisualStudioPath)
    if (-not $VisualStudioPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe not found.' }
        $VisualStudioPath = & $vswhere -version '[17.0,18.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or -not $VisualStudioPath) { throw 'Visual Studio 2022 C++ tools not found.' }
    }
    $devcmd = Join-Path $VisualStudioPath 'Common7/Tools/VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devcmd)) { throw 'Visual Studio developer command script not found.' }
    # 不使用 set 的逐行文本，环境值中的换行和等号也必须完整保留。
    $export = 'foreach($item in [Environment]::GetEnvironmentVariables("Process").GetEnumerator()){[Console]::WriteLine([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes([string]$item.Key))+":"+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes([string]$item.Value)))}'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($export))
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = Join-Path $env:SystemRoot 'System32/cmd.exe'
    $info.Arguments = '/d /s /v:off /c ""%SELFENGINE_VSDEVCMD%" -no_logo -arch=amd64 -host_arch=amd64 >nul && "%SELFENGINE_ENV_EXPORTER%" -NoLogo -NoProfile -NonInteractive -EncodedCommand ' + $encoded + '"'
    $info.EnvironmentVariables['SELFENGINE_VSDEVCMD'] = $devcmd
    $info.EnvironmentVariables['SELFENGINE_ENV_EXPORTER'] = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::ASCII
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'Unable to start the MSVC environment loader.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $payload = $stdout.GetAwaiter().GetResult()
        $null = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw ('MSVC environment initialization failed (exit code ' + $process.ExitCode + ').') }
        $environment = @{}
        foreach ($line in ($payload -split '\r?\n')) {
            if (-not $line) { continue }
            $parts = $line.Split(':')
            if ($parts.Length -ne 2) { throw 'Invalid MSVC environment export.' }
            $name = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($parts[0]))
            $value = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($parts[1]))
            if ($name.StartsWith('=')) { continue }
            if ($name -eq 'SELFENGINE_VSDEVCMD' -or $name -eq 'SELFENGINE_ENV_EXPORTER') { continue }
            $environment[$name] = $value
        }
        if ($environment['VSCMD_ARG_TGT_ARCH'] -ne 'x64' -or -not $environment['VCINSTALLDIR']) { throw 'MSVC x64 environment was not initialized.' }
        # 完整解码并检查成功后才修改当前进程；不持久化环境或输出可能含秘密的值。
        foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process') }
    } finally { $process.Dispose() }
}
