param(
    [string]$Project='',
    [ValidateSet('zh-CN','en-US')][string]$Language='zh-CN',
    [switch]$SafeMode,
    [ValidateSet('Debug','Release')][string]$Configuration='Release'
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$configuration=$Configuration.ToLowerInvariant()
$exe=Join-Path $root ('out/build/windows-x64-'+$configuration+'/FlareEditor.exe')
if(!(Test-Path -LiteralPath $exe)){throw ('FlareEditor not built: '+$exe+' (run cmake --build --preset windows-x64-'+$configuration+' first)')}
$arguments=@('--language',$Language)
if($Project){$arguments=@('--project',$Project)+$arguments}
if($SafeMode){$arguments+='--safe-mode'}
& $exe @arguments
exit $LASTEXITCODE
