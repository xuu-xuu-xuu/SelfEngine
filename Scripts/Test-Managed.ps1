param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [string]$NativeDirectory = '',
    [switch]$WindowSmoke
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Copy-RendererResources.ps1')
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $projectRoot
if ([string]::IsNullOrWhiteSpace($NativeDirectory)) {
    $NativeDirectory = Join-Path $projectRoot ('out/build/windows-x64-' + $Configuration.ToLowerInvariant())
}
$nativeLibrary = Join-Path $NativeDirectory 'EngineNative.dll'
if (!(Test-Path -LiteralPath $nativeLibrary)) { throw 'EngineNative.dll is missing; build the native CMake target first.' }
$projectPaths = @{
    ManagedTests = 'Tests/Managed/ManagedTests/ManagedTests.csproj'
    MenuFlowTests = 'Tests/Managed/MenuFlowTests/MenuFlowTests.csproj'
    Bootstrap = 'Samples/Managed/Bootstrap/Bootstrap.csproj'
}
foreach ($projectName in @('ManagedTests','MenuFlowTests','Bootstrap')) {
    $projectFile = Join-Path $projectRoot $projectPaths[$projectName]
    dotnet restore $projectFile --configfile (Join-Path $projectRoot 'NuGet.Config') --packages (Join-Path $projectRoot 'out/nuget/packages') --nologo
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    dotnet build $projectFile --configuration $Configuration --no-restore --nologo
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $binaryDirectory = Join-Path $projectRoot ('out/managed/' + $projectName + '/' + $Configuration + '/net10.0')
    Copy-Item -LiteralPath $nativeLibrary -Destination (Join-Path $binaryDirectory 'EngineNative.dll') -Force
    $resources = Join-Path $binaryDirectory 'Resources'
    New-Item -ItemType Directory -Path $resources -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'Resources/Localization') -Destination $resources -Recurse -Force
    Copy-EngineRendererResources -NativeDirectory $NativeDirectory -Destination $resources
    $program = Join-Path $binaryDirectory ('SelfEngine.' + $projectName + '.dll')
    if ($WindowSmoke -and $projectName -in @('ManagedTests','MenuFlowTests')) {
        $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try { & dotnet $program --window-smoke; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $previous }
    }
    else {
        $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try { & dotnet $program; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $previous }
    }
    if ($code -ne 0) { exit $code }
}
exit 0
