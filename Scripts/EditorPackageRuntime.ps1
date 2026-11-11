# 便携编辑器与源码导出共用的运行输入适配。调用方负责输出目录和构建租约。
function Get-EngineScriptInputs([string]$ScriptProject) {
    $directory=Split-Path -Parent ([IO.Path]::GetFullPath($ScriptProject))
    $files=@(Get-ChildItem -LiteralPath $directory -File -Recurse -Force | Where-Object {
        $_.FullName.Substring($directory.Length+1) -notmatch '(^|[\\/])(bin|obj|out|\.cache|\.git)([\\/]|$)'
    } | Sort-Object FullName)
    return @(foreach($file in $files){
        $entry=$file
        while($entry -and $entry.FullName -ne $directory){
            if($entry.Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('Export.PathOutsideRoot '+$file.FullName)}
            if($entry -is [IO.DirectoryInfo]){$entry=$entry.Parent}else{$entry=$entry.Directory}
        }
        [ordered]@{path=$file.FullName.Substring($directory.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
}

function Read-EngineEditorPackage([string]$Root) {
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
    $manifestPath=Join-Path $Root 'editor-package.json'
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if($manifest.schema -ne 1 -or $manifest.kind -ne 'SelfEngine.EditorPackage' -or $manifest.export_contract -ne 1){throw 'Export.EditorPackageInvalid'}
    $listed=@{}
    foreach($file in $manifest.files){
        if(!$file.path -or [IO.Path]::IsPathRooted($file.path) -or $listed.ContainsKey($file.path)){throw 'Export.EditorPackageInvalid'}
        $path=[IO.Path]::GetFullPath((Join-Path $Root $file.path))
        if(!$path.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Export.PathOutsideRoot'}
        $entry=Get-Item -LiteralPath $path -ErrorAction Stop
        if($entry -isnot [IO.FileInfo] -or $entry.Length -ne $file.bytes){throw ('Export.EditorPackageInvalid '+$file.path)}
        while($entry -and $entry.FullName -ne $Root){
            if($entry.Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('Export.PathOutsideRoot '+$file.path)}
            if($entry -is [IO.DirectoryInfo]){$entry=$entry.Parent}else{$entry=$entry.Directory}
        }
        if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){throw ('Export.SourceChanged '+$file.path)}
        $listed[$file.path]=$true
    }
    foreach($file in Get-ChildItem -LiteralPath $Root -File -Recurse -Force){
        $relative=$file.FullName.Substring($Root.Length+1).Replace('\','/')
        if($relative -ne 'editor-package.json' -and !$listed.ContainsKey($relative)){throw ('Export.EditorPackageInvalid '+$relative)}
    }
    foreach($required in @('Tools/ProjectTool.exe','Player/SelfEngine.Bootstrap.exe','Player/EngineNative.dll','Player/SelfEngine.Managed.dll',
        'Player/SelfEngine.Bootstrap.runtimeconfig.json','SDK/Managed/SelfEngine.Managed.dll','SDK/SelfEngine.Scripts.props','SDK/SelfEngine.Scripts.targets')){
        if(!$listed.ContainsKey($required)){throw ('Export.EditorPackageInvalid '+$required)}
    }
    return [ordered]@{schema=1;identity='editor-package.json plus verified file hashes';git_head=$manifest.git_head;dirty=$manifest.source_dirty;
        source_sha256=$manifest.source_sha256;editor_package_sha256=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant();files=$manifest.files}
}

function Find-EnginePrecompiledScripts([string]$Root,[string]$ScriptProject) {
    $directory=Join-Path $Root 'SDK/Precompiled/EmptyGame'
    $path=Join-Path $directory 'manifest.json'
    if(!(Test-Path -LiteralPath $path -PathType Leaf)){return $null}
    $manifest=Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    if($manifest.schema -ne 1 -or $manifest.project -ne 'EmptyGame.Scripts.csproj' -or $manifest.project -ne [IO.Path]::GetFileName($ScriptProject)){return $null}
    $actual=@(Get-EngineScriptInputs $ScriptProject)
    if($actual.Count -ne @($manifest.source_files).Count){return $null}
    $expected=@{};foreach($file in $manifest.source_files){$expected[$file.path]=$file.sha256}
    # 仅固定模板的这份 JSON 是独立的运行时字段文档；它不参与程序集生成。
    # 工程和两份目录构建文件必须仍与包内模板逐字相同，不能把该例外推广给资源 JSON。
    foreach($required in @('EmptyGame.Scripts.csproj','Directory.Build.props','Directory.Build.targets','Main.bindings.json')){
        if(!$expected.ContainsKey($required)){return $null}
    }
    foreach($file in $actual){
        if(!$expected.ContainsKey($file.path)){return $null}
        if($file.path -eq 'Main.bindings.json'){continue}
        if($expected[$file.path] -ne $file.sha256){return $null}
    }
    if(!$manifest.assembly -or $manifest.assembly.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0){throw 'Export.EditorPackageInvalid'}
    return [pscustomobject]@{Directory=(Join-Path $directory 'Files');Assembly=[string]$manifest.assembly;SourceFiles=$actual}
}

function Get-EnginePackageScriptProperties([string]$Root,[string]$IntermediateDirectory) {
    # CustomAfterMicrosoftCommonTargets 也兼容旧模板里的 Engine.Managed.csproj 引用。
    # 只替换引擎 API 引用，项目自己的 ProjectReference 不受影响。
    return @(('-p:SelfEngineRoot='+$Root),('-p:CustomAfterMicrosoftCommonTargets='+(Join-Path $Root 'SDK/SelfEngine.Scripts.targets')),
        ('-p:BaseIntermediateOutputPath='+([IO.Path]::GetFullPath($IntermediateDirectory).TrimEnd('\')+'\')),
        ('-p:MSBuildProjectExtensionsPath='+([IO.Path]::GetFullPath($IntermediateDirectory).TrimEnd('\')+'\')))
}

function Remove-EngineExportStage([string]$Stage,[string]$OutputRoot,[string]$Name) {
    if(!$Stage -or !(Test-Path -LiteralPath $Stage)){return}
    $root=[IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
    $target=[IO.Path]::GetFullPath($Stage)
    if([IO.Path]::GetDirectoryName($target) -ne $root -or ![IO.Path]::GetFileName($target).StartsWith('.'+$Name+'.stage-',[StringComparison]::Ordinal)){
        throw 'Export.PathOutsideRoot'
    }
    if((Get-Item -LiteralPath $target -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Export.PathOutsideRoot'}
    Remove-Item -LiteralPath $target -Recurse -Force
}
