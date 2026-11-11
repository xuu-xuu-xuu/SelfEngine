# UI layering guard: scans UI core/widget includes and CMake target link direction.
# Registered as the UiLayering CTest case; any violation exits with 1.
[CmdletBinding()]
param(
    [string]$Root
)

$ErrorActionPreference = 'Stop'

if (-not $Root)
{
    $Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}

function Read-TextFile([string]$Path)
{
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function To-PosixPath([string]$Path)
{
    return $Path.Replace('\', '/')
}

$rootPath = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
$violations = New-Object 'System.Collections.Generic.List[string]'
$scannedFiles = 0
$checkedTargets = 0

function Add-Violation([string]$Message)
{
    $script:violations.Add($Message)
}

$uiRoot = Join-Path $rootPath 'Source/FunctionLayer/UI'
$sourceFiles = @()
if (Test-Path -LiteralPath $uiRoot)
{
    $sourceFiles += @(Get-ChildItem -LiteralPath $uiRoot -Recurse -File |
        Where-Object { $_.Extension -in @('.h', '.hpp', '.cpp', '.cc') })
}
$renderingRoot = Join-Path $rootPath 'Source/FunctionLayer/Rendering'
if (Test-Path -LiteralPath $renderingRoot)
{
    $sourceFiles += @(Get-ChildItem -LiteralPath $renderingRoot -File |
        Where-Object { $_.Extension -in @('.h', '.hpp', '.cpp', '.cc') -and $_.Name -match '^Ui(Paint|Renderer)\.' })
}
$editorUiRoot = Join-Path $rootPath 'Source/ToolLayer/Editor/EditorUi'
if (Test-Path -LiteralPath $editorUiRoot)
{
    $sourceFiles += @(Get-ChildItem -LiteralPath $editorUiRoot -Recurse -File |
        Where-Object { $_.Extension -in @('.h', '.hpp', '.cpp', '.cc') })
}

$coreForbidden = '(^|/)(windows\.h|d3d12[^/]*\.h|dxgi[^/]*\.h|imgui[^/]*\.h|uitree\.h)(?:$|/)|(^|/)(toollayer|resourcelayer|platformlayer|thirdparty)(/|$)|(^|/)(editor|gameui)(/|$)'
$renderForbidden = '(^|/)(windows\.h|d3d12[^/]*\.h|imgui[^/]*\.h|uitree\.h)(?:$|/)|(^|/)(toollayer|thirdparty|resourcelayer)(/|$)|(^|/)(editor|gameui)(/|$)|^platformlayer/graphics/d3d12(/|$)'
$editorForbidden = '(^|/)(imgui[^/]*\.h|d3d12[^/]*\.h|dxgi[^/]*\.h|editorsession\.h|world\.h|assetstore\.h)(?:$|/)|^platformlayer/graphics/d3d12(/|$)'
$symbolForbidden = '\bImGui::|\bimgui_|\bID3D12|\bIDXGI|\bHWND\b|\bHINSTANCE\b|\bIDWrite|\bDirectWrite\b|\bdwrite\b|\bAssetStore\b|\bEditorSession\b|\bUiTree\b'
$ignoreCase = [System.Text.RegularExpressions.RegexOptions]::IgnoreCase

foreach ($file in $sourceFiles)
{
    $scannedFiles++
    $relative = To-PosixPath($file.FullName.Substring($rootPath.Length).TrimStart('\', '/'))
    $isCore = $relative -match '^Source/FunctionLayer/UI/(Core|Interfaces)/'
    $isWidgets = $relative -match '^Source/FunctionLayer/UI/Widgets/'
    $isRenderUi = $relative -match '^Source/FunctionLayer/Rendering/Ui(Paint|Renderer)\.'
    $isEditorUi = $relative -match '^Source/ToolLayer/Editor/EditorUi/'
    if (-not ($isCore -or $isWidgets -or $isRenderUi -or $isEditorUi))
    {
        continue
    }

    $lines = (Read-TextFile $file.FullName) -split "`r?`n"
    for ($index = 0; $index -lt $lines.Count; $index++)
    {
        $lineNumber = $index + 1
        $line = $lines[$index]
        $includeMatch = [regex]::Match($line, '^\s*#\s*include\s*[<"]([^>"]+)[>"]')
        if ($includeMatch.Success)
        {
            $include = $includeMatch.Groups[1].Value
            $pattern = if ($isRenderUi) { $renderForbidden } elseif ($isEditorUi) { $editorForbidden } else { $coreForbidden }
            if ([regex]::IsMatch($include, $pattern, $ignoreCase))
            {
                Add-Violation ("{0}:{1}: forbidden include '{2}'" -f $relative, $lineNumber, $include)
            }
            continue
        }

        if ($isCore -or $isWidgets)
        {
            $code = $line -replace '//.*$', ''
            if ([regex]::IsMatch($code, $symbolForbidden))
            {
                Add-Violation ("{0}:{1}: forbidden platform/editor symbol" -f $relative, $lineNumber)
            }
        }
    }
}

# Public-header firewall: check each frozen header that already exists.
$publicHeaders = @(
    'Source/FunctionLayer/UI/Core/UiElement.h',
    'Source/FunctionLayer/UI/Core/UiDocument.h',
    'Source/FunctionLayer/UI/Core/UiStyle.h',
    'Source/FunctionLayer/UI/Core/UiLayout.h',
    'Source/FunctionLayer/UI/Interfaces/UiTextService.h',
    'Source/FunctionLayer/UI/Interfaces/UiImageProvider.h',
    'Source/FunctionLayer/UI/Interfaces/UiPlatformServices.h',
    'Source/FunctionLayer/UI/Interfaces/UiPaintSink.h',
    'Source/FunctionLayer/UI/Core/UiFrameData.h'
)
foreach ($publicHeader in $publicHeaders)
{
    $fullPath = Join-Path $rootPath $publicHeader
    if (-not (Test-Path -LiteralPath $fullPath))
    {
        continue
    }
    $lines = (Read-TextFile $fullPath) -split "`r?`n"
    for ($index = 0; $index -lt $lines.Count; $index++)
    {
        $includeMatch = [regex]::Match($lines[$index], '^\s*#\s*include\s*[<"]([^>"]+)[>"]')
        if (-not $includeMatch.Success)
        {
            continue
        }
        $include = $includeMatch.Groups[1].Value
        if ([regex]::IsMatch($include, $coreForbidden, $ignoreCase))
        {
            Add-Violation ("{0}:{1}: public header includes forbidden '{2}'" -f $publicHeader, ($index + 1), $include)
        }
    }
}

# CMake target graph. Project target_link_libraries lines are single-line.
$cmakePath = Join-Path $rootPath 'CMakeLists.txt'
$cmake = Read-TextFile $cmakePath
$links = @{}
$linkRegex = [regex]'(?m)^\s*target_link_libraries\s*\(\s*([A-Za-z0-9_]+)\s+([^)]*)\)'
foreach ($match in $linkRegex.Matches($cmake))
{
    $target = $match.Groups[1].Value
    if (-not $links.ContainsKey($target))
    {
        $links[$target] = @()
    }
    foreach ($token in ($match.Groups[2].Value -split '\s+'))
    {
        if (-not $token)
        {
            continue
        }
        if ($token -match '^(?i)(public|private|interface|debug|optimized|general)$')
        {
            continue
        }
        if ($token.StartsWith('$'))
        {
            continue
        }
        $links[$target] = @($links[$target]) + $token
    }
}

$allowedTargets = @{
    'EngineUiCore'    = @('EngineCore')
    'EngineUiWidgets' = @('EngineUiCore', 'EngineCore')
    'EngineUiRender'  = @('EngineUiCore', 'EngineCore', 'EngineGraphics')
    'EngineEditorUi'  = @('EngineUiCore', 'EngineUiWidgets', 'EngineCore', 'EngineRuntime', 'EngineAssets', 'EngineGraphics', 'EnginePlatform', 'user32', 'imm32')
}
$forbiddenTargets = @{
    'EngineUiCore'    = @('EnginePlatform', 'EngineGraphics', 'EngineAssets', 'EngineRuntime', 'EngineEditor', 'EngineEditorUi', 'EngineEditorApp', 'EngineEditorProduct', 'EngineUiRender', 'EngineUiWidgets', 'imgui', 'EngineSandboxRenderer')
    'EngineUiWidgets' = @('EnginePlatform', 'EngineGraphics', 'EngineAssets', 'EngineRuntime', 'EngineEditor', 'EngineEditorUi', 'EngineEditorApp', 'EngineEditorProduct', 'EngineUiRender', 'imgui', 'EngineSandboxRenderer')
    'EngineUiRender'  = @('EngineEditor', 'EngineEditorUi', 'EngineEditorApp', 'EngineEditorProduct', 'EngineUiWidgets', 'EngineAssets', 'EngineRuntime', 'imgui', 'EngineSandboxRenderer')
    'EngineEditorUi'  = @('EngineEditor', 'EngineEditorApp', 'EngineEditorProduct', 'imgui', 'EngineSandboxRenderer')
    'EnginePlatform'  = @('EngineEditor', 'EngineEditorUi', 'EngineEditorApp', 'EngineEditorProduct', 'imgui')
}
foreach ($target in $forbiddenTargets.Keys)
{
    if (-not $links.ContainsKey($target))
    {
        continue
    }
    $checkedTargets++
    foreach ($dependency in $links[$target])
    {
        if ($forbiddenTargets[$target] -contains $dependency)
        {
            Add-Violation ("CMake target '{0}' must not link '{1}'" -f $target, $dependency)
        }
        if ($allowedTargets.ContainsKey($target) -and ($allowedTargets[$target] -notcontains $dependency))
        {
            Add-Violation ("CMake target '{0}' links unexpected '{1}'" -f $target, $dependency)
        }
    }
}

$standaloneTests = @{
    'UiCoreStandaloneTests' = @('EngineUiCore', 'EngineCore')
    'UiApiConsumerTests'    = @('EngineUiCore', 'EngineCore')
    'UiCoreDocumentTests'   = @('EngineUiCore', 'EngineCore')
    'UiCoreLoaderTests'     = @('EngineUiCore', 'EngineCore')
    'UiCoreBindingTests'    = @('EngineUiCore', 'EngineCore')
    'UiCoreBenchmark'       = @('EngineUiCore', 'EngineCore')
    'UiCoreLayoutTests'     = @('EngineUiCore', 'EngineCore')
    'UiCoreLayoutBenchmark' = @('EngineUiCore', 'EngineCore')
    'UiCoreStyleTests'      = @('EngineUiCore', 'EngineCore')
    'UiCoreTextTests'       = @('EngineUiCore', 'EnginePlatform', 'EngineCore')
    'UiCorePaintTests'      = @('EngineUiRender', 'EngineUiCore', 'EngineCore')
    'UiCoreInputTests'      = @('EngineUiCore', 'EngineCore')
    'UiCoreWidgetsTests'    = @('EngineUiWidgets', 'EngineUiCore', 'EngineCore')
    'UiWidgetsDemo'         = @('EngineUiWidgets', 'EngineUiRender', 'EngineUiCore', 'EngineCore')
    'EditorUiTests'         = @('EngineEditorUi')
    'EditorUiConsumerTests' = @('EngineEditorUi')
    'EditorShellTests'      = @('EngineEditorUi')
    'EditorPanelsTests'     = @('EngineEditorUi')
    'EditorPanelSessionTests' = @('EngineEditorUi', 'EngineEditor')
    'EditorViewportTests'   = @('EngineEditorUi')
}
foreach ($testTarget in $standaloneTests.Keys)
{
    if (-not $links.ContainsKey($testTarget))
    {
        Add-Violation ("missing CMake target '{0}'" -f $testTarget)
        continue
    }
    $checkedTargets++
    foreach ($dependency in $links[$testTarget])
    {
        if ($standaloneTests[$testTarget] -notcontains $dependency)
        {
            Add-Violation ("{0} must only link EngineUiCore/EngineCore, found '{1}'" -f $testTarget, $dependency)
        }
    }
}

if ($cmake -notmatch 'add_library\s*\(\s*EngineUiCore\b')
{
    Add-Violation 'CMake is missing the EngineUiCore target'
}
foreach ($testName in @('UiLayering', 'UiCoreStandalone', 'UiApiConsumer', 'UiCoreDocument', 'UiCoreLoader', 'UiCoreBinding', 'UiCoreBenchmark', 'UiCoreLayout', 'UiCoreLayoutBenchmark', 'UiCoreStyle', 'UiCoreText', 'UiPaint', 'UiInput', 'UiWidgets', 'UiWidgetsDemo', 'EditorUi', 'EditorUiConsumer', 'EditorShell', 'EditorPanels', 'EditorPanelSession', 'EditorViewport'))
{
    if ($cmake -notmatch ('(?m)add_test\s*\(\s*NAME\s+' + [regex]::Escape($testName) + '\b'))
    {
        Add-Violation ("CMake is missing add_test(NAME {0} ...)" -f $testName)
    }
}

Write-Output ("UI layering check: scanned {0} UI source file(s), checked {1} target(s)." -f $scannedFiles, $checkedTargets)
if ($violations.Count -gt 0)
{
    Write-Output ("UI layering check FAILED: {0} violation(s)." -f $violations.Count)
    foreach ($violation in $violations)
    {
        Write-Output ("  - " + $violation)
    }
    exit 1
}
Write-Output 'UI layering check passed: 0 violations.'
exit 0
