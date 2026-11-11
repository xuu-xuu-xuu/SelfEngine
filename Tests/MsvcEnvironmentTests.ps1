param([string]$OutputDirectory='out/validation/msvc-environment')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'Scripts/Import-MsvcEnvironment.ps1')
$directory=[IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if(-not $directory.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Test output must stay inside the workspace.'}
if(Test-Path -LiteralPath $directory){throw 'Use a new test output directory.'}
New-Item -ItemType Directory -Path $directory | Out-Null
$saved=[Environment]::GetEnvironmentVariables('Process')
$encoding=[Console]::OutputEncoding.CodePage
$location=(Get-Location).Path
try {
    # 使用字符码避免 Windows PowerShell 5.1 对无 BOM 脚本的解码影响夹具。
    $probe=[string][char]0x7d6e+[char]::ConvertFromUtf32(0x1f642)+' = & % !'+[Environment]::NewLine+'second=line'
    $env:SELFENGINE_ENV_PROBE=$probe
    Import-EngineMsvcEnvironment
    if($env:SELFENGINE_ENV_PROBE -cne $probe){throw 'Unicode or multiline environment value changed.'}
    foreach($name in @('USERPROFILE','APPDATA','LOCALAPPDATA','PSModulePath')){
        if([Environment]::GetEnvironmentVariable($name,'Process') -cne $saved[$name]){throw ('Caller environment changed: '+$name)}
    }
    if([Console]::OutputEncoding.CodePage -ne $encoding -or (Get-Location).Path -ne $location){throw 'Caller console encoding or directory changed.'}
    foreach($tool in @('cl.exe','link.exe','cmake.exe','ninja.exe','ctest.exe')){if(-not (Get-Command $tool -ErrorAction SilentlyContinue)){throw ('Tool not found: '+$tool)}}
    $beforeFailure=[Environment]::GetEnvironmentVariables('Process')
    $fake=Join-Path $directory 'failing VS/Common7/Tools'
    New-Item -ItemType Directory -Path $fake | Out-Null
    [IO.File]::WriteAllText((Join-Path $fake 'VsDevCmd.bat'),"@set SELFENGINE_ENV_PROBE=broken`r`n@exit /b 7`r`n",[Text.Encoding]::ASCII)
    $rejected=$false
    try{Import-EngineMsvcEnvironment -VisualStudioPath (Join-Path $directory 'failing VS')}catch{if($_.Exception.Message -notlike '*exit code 7*'){throw};$rejected=$true}
    if(-not $rejected){throw 'Failed setup was accepted.'}
    $afterFailure=[Environment]::GetEnvironmentVariables('Process')
    if($beforeFailure.Count -ne $afterFailure.Count){throw 'Failed setup changed environment keys.'}
    foreach($name in $beforeFailure.Keys){if($beforeFailure[$name] -cne $afterFailure[$name]){throw 'Failed setup partially imported environment.'}}
    $proof=[ordered]@{success=$true;powershell=$PSVersionTable.PSVersion.ToString();console_codepage=$encoding;unicode=$true;surrogate_pair=$true;multiline=$true;caller_paths_preserved=$true;failed_setup_atomic=$true;x64_tools=$true}
    [IO.File]::WriteAllText((Join-Path $directory 'verification.json'),($proof|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    $proof|ConvertTo-Json -Compress
} finally {
    foreach($name in [Environment]::GetEnvironmentVariables('Process').Keys){if(-not $saved.Contains($name)){[Environment]::SetEnvironmentVariable($name,$null,'Process')}}
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
