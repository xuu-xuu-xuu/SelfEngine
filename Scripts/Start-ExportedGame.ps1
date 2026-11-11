param([switch]$Headless,[switch]$Hidden,[switch]$Editor,[switch]$Play,[ValidateRange(0,100000)][int]$Frames=0,[string]$Capture='',[ValidateRange(1,100000)][int]$CaptureFrame=1,[ValidateSet('zh-CN','en-US')][string]$Locale='zh-CN')
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
$ErrorActionPreference='Stop'
function PackageFile([string]$Relative) {
    if([IO.Path]::IsPathRooted($Relative)){throw 'Export.PathOutsideRoot'}
    $path=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot $Relative))
    if(!$path.StartsWith($PSScriptRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Export.PathOutsideRoot'}
    return $path
}
$settings=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'launch.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($Headless -and $Editor){throw 'Bridge.InvalidArgument'}
$arguments=@('--project-root',(PackageFile 'Project'),'--scene',(PackageFile $settings.scene),'--asset-root',(PackageFile $settings.assetRoot),'--asset-cache',(PackageFile $settings.assetCache),'--project',(PackageFile $settings.scriptAssembly),'--script',$settings.scriptType,'--locale',$Locale)
if($settings.menu){$arguments+='--menu'}
if($settings.bindings){$arguments+=@('--bindings',(PackageFile $settings.bindings))}
if($settings.ui){$arguments+=@('--ui',(PackageFile $settings.ui))}
if($Editor){$arguments+='--editor';if($Play){$arguments+='--play'}}elseif(!$Headless){$arguments+='--window'}
if($Hidden){$arguments+='--hidden'}
if($Frames -gt 0){$arguments+=@('--frames',[string]$Frames)}
if($Capture){if($Headless -or $Editor){throw 'Bridge.InvalidArgument'};$arguments+=@('--capture',[IO.Path]::GetFullPath($Capture),'--capture-frame',[string]$CaptureFrame)}
& (Join-Path $PSScriptRoot 'SelfEngine.Bootstrap.exe') @arguments
exit $LASTEXITCODE
