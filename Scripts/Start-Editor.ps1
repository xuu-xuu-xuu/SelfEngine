param(
    [string]$Project='',
    [ValidateSet('zh-CN','en-US')][string]$Language='zh-CN',
    [switch]$SafeMode,
    [ValidateSet('Debug','Release')][string]$Configuration='Debug'
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$configuration=$Configuration.ToLowerInvariant()
$exe=Join-Path $root ('out/build/windows-x64-'+$configuration+'/SelfEngineEditor.exe')
if(!(Test-Path -LiteralPath $exe)){throw ('SelfEngineEditor not built: '+$exe+' (run cmake --build --preset windows-x64-'+$configuration+' first)')}
$arguments=@('--language',$Language)
if($Project){$arguments=@('--project',$Project)+$arguments}
if($SafeMode){$arguments+='--safe-mode'}
& $exe @arguments
exit $LASTEXITCODE
