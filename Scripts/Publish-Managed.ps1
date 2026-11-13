param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [switch]$FrameworkDependent,[string]$OutputDirectory='',[string]$ManagedOutputRoot='')
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Copy-RendererResources.ps1')
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $projectRoot
$nativeDirectory = Join-Path $projectRoot ('out/build/windows-x64-' + $Configuration.ToLowerInvariant())
$nativeLibrary = Join-Path $nativeDirectory 'EngineNative.dll'
if (!(Test-Path -LiteralPath $nativeLibrary)) { throw 'EngineNative.dll is missing; build native targets first.' }
$publishDirectory = if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $projectRoot ('out/publish/managed-' + $Configuration.ToLowerInvariant())}
if($OutputDirectory -and (Test-Path -LiteralPath $publishDirectory)){throw 'Use a new publish directory.'}
$selfContained = if ($FrameworkDependent) { 'false' } else { 'true' }
$nugetRoot=Join-Path $projectRoot '.cache/nuget'
New-Item -ItemType Directory -Path $nugetRoot -Force | Out-Null
$env:NUGET_PACKAGES=Join-Path $nugetRoot 'packages'
$env:NUGET_HTTP_CACHE_PATH=Join-Path $nugetRoot 'http-cache'
$env:DOTNET_CLI_HOME=Join-Path $nugetRoot 'home'
# 导出/打包时把中间输出放到调用方唯一目录，避免与正在运行的编辑器共享 out/managed。
$baseArgs=@()
if($ManagedOutputRoot){
    $managedRoot=[IO.Path]::GetFullPath($ManagedOutputRoot)
    New-Item -ItemType Directory -Path $managedRoot -Force | Out-Null
    $baseArgs=@("-p:SelfEngineManagedOutputRoot=$managedRoot/","-p:SelfEngineManagedIntermediateRoot=$managedRoot/obj/")
}
dotnet restore Source/FunctionLayer/Scripting/Bootstrap/Bootstrap.csproj --runtime win-x64 --configfile BuildConfig/NuGet.Config --packages out/nuget/packages "-p:Configuration=$Configuration" "-p:SelfContained=$selfContained" @baseArgs --nologo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
dotnet publish Source/FunctionLayer/Scripting/Bootstrap/Bootstrap.csproj --configuration $Configuration --runtime win-x64 --self-contained $selfContained --output $publishDirectory -p:PublishSingleFile=false -p:PublishTrimmed=false @baseArgs --no-restore --nologo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -LiteralPath $nativeLibrary -Destination (Join-Path $publishDirectory 'EngineNative.dll') -Force
if (-not $FrameworkDependent) {
    $runtimeConfig = Get-Content -LiteralPath (Join-Path $publishDirectory 'SelfEngine.Bootstrap.runtimeconfig.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $runtime = @($runtimeConfig.runtimeOptions.includedFrameworks | Where-Object name -eq 'Microsoft.NETCore.App')
    if ($runtime.Count -ne 1 -or $runtime[0].version -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$') { throw 'Cannot identify the published .NET runtime version.' }
    $runtimePackage = Join-Path $projectRoot ('out/nuget/packages/microsoft.netcore.app.runtime.win-x64/' + $runtime[0].version)
    $notices = Join-Path $publishDirectory 'ThirdParty/dotnet'
    New-Item -ItemType Directory -Force -Path $notices | Out-Null
    foreach ($name in @('LICENSE.TXT','THIRD-PARTY-NOTICES.TXT')) {
        Copy-Item -LiteralPath (Join-Path $runtimePackage $name) -Destination (Join-Path $notices $name) -Force
    }
}
$localization = Join-Path $projectRoot 'Resources/Localization'
$resourceDestination = Join-Path $publishDirectory 'Resources'
New-Item -ItemType Directory -Path $resourceDestination -Force | Out-Null
Copy-Item -LiteralPath $localization -Destination $resourceDestination -Recurse -Force
Copy-EngineRendererResources -NativeDirectory $nativeDirectory -Destination $resourceDestination
Write-Output $publishDirectory
exit 0
