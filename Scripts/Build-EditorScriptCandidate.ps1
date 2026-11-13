param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [Parameter(Mandatory=$true)][string]$NativeDirectory,
    [Parameter(Mandatory=$true)][string]$OperationDirectory,
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [ValidateSet('zh-CN','en-US')][string]$Locale='zh-CN'
)
# 正式编辑器监督进程调用。候选发布前不修改上次成功版本，也不启动编辑世界。
$ErrorActionPreference='Stop'
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'ProjectBuildLease.ps1')
. (Join-Path $PSScriptRoot 'EditorPackageRuntime.ps1')
. (Join-Path $PSScriptRoot 'Copy-RendererResources.ps1')
$engineRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$ProjectRoot=[IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\')
$NativeDirectory=[IO.Path]::GetFullPath($NativeDirectory).TrimEnd('\')
$OperationDirectory=[IO.Path]::GetFullPath($OperationDirectory).TrimEnd('\')
$packageMode=Test-Path -LiteralPath (Join-Path $engineRoot 'editor-package.json') -PathType Leaf
$nugetConfig=if($packageMode){Join-Path $engineRoot 'NuGet.Config'}else{Join-Path $engineRoot 'BuildConfig/NuGet.Config'}
$sourceExtensions=@('.cs','.csproj','.props','.targets','.lock.json')
$sourceFileNames=@('build.json','global.json','NuGet.Config')
$sourceFiles=@();$sourceIdentity='';$engineIdentity='';$lease=$null;$candidate=$null;$failureKey='Preview.BuildFailed'
$status=[ordered]@{schemaVersion=1;state='failed';managed=$true;hasCandidate=$false;sourceIdentity='';candidateIdentity='';candidateRoot='';diagnosticKey='';diagnosticsPath=(Join-Path $OperationDirectory 'build.log');sourceDirectories=@('.');sourceExtensions=$sourceExtensions;sourceFileNames=$sourceFileNames;sourceFiles=@();scriptsProject='';configuration=$Configuration}
$progress=[ordered]@{schemaVersion=1;phase='input';logPath=''}
$utf8=New-Object Text.UTF8Encoding($false)
function WriteJson([string]$Path,$Value) {
    $temporary=$Path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 20),$utf8)
        if(Test-Path -LiteralPath $Path){[IO.File]::Replace($temporary,$Path,[NullString]::Value)}else{[IO.File]::Move($temporary,$Path)}
    }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
}
function HashText([string]$Text) {
    $hash=[Security.Cryptography.SHA256]::Create()
    try{return [BitConverter]::ToString($hash.ComputeHash($utf8.GetBytes($Text))).Replace('-','').ToLowerInvariant()}finally{$hash.Dispose()}
}
function ProjectFile([string]$Relative) {
    if(!$Relative -or [IO.Path]::IsPathRooted($Relative)){throw 'Preview.InvalidProject'}
    $path=[IO.Path]::GetFullPath((Join-Path $ProjectRoot $Relative))
    if(!$path.StartsWith($ProjectRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $path -PathType Leaf)){throw ('Preview.InvalidProject '+$Relative)}
    $entry=Get-Item -LiteralPath $path -Force
    while($entry -and $entry.FullName -ne $ProjectRoot){
        if($entry.Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('Export.PathOutsideRoot '+$Relative)}
        if($entry -is [IO.DirectoryInfo]){$entry=$entry.Parent}else{$entry=$entry.Directory}
    }
    return $path
}
function SourceInputs {
    $pending=New-Object 'Collections.Generic.Stack[string]';$pending.Push($ProjectRoot)
    $included=New-Object 'Collections.Generic.List[IO.FileInfo]'
    while($pending.Count){
        foreach($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force){
            if($entry -is [IO.DirectoryInfo]){
                if($entry.Name -in @('out','bin','obj','.git','.vs','.cache')){continue}
                if($entry.Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('Export.PathOutsideRoot '+$entry.FullName)}
                $pending.Push($entry.FullName);continue
            }
            $match=$entry.Name -in $sourceFileNames
            foreach($extension in $sourceExtensions){if($entry.Name.EndsWith($extension,[StringComparison]::OrdinalIgnoreCase)){$match=$true}}
            if($match){$included.Add($entry)}
        }
    }
    return @($included | Sort-Object FullName | ForEach-Object {
        $relative=$_.FullName.Substring($ProjectRoot.Length+1).Replace('\','/')
        $path=ProjectFile $relative
        [ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
}
function Step([string]$Phase,[string]$Executable,[string[]]$Arguments) {
    $log=Join-Path $OperationDirectory ($Phase+'.log')
    $progress.phase=$Phase;$progress.logPath=$log;WriteJson (Join-Path $OperationDirectory 'build-progress.json') $progress
    Write-Output ('['+$Phase+'] '+$log)
    $code=Invoke-EngineProgram $Executable $Arguments $log
    # 完整编译器文件/行号同时保留在步骤日志和统一诊断日志。
    if(Test-Path -LiteralPath $log){Write-Output (Get-Content -LiteralPath $log -Raw -Encoding UTF8)}
    if($code -ne 0){throw ($failureKey+' '+$Phase+' exit='+$code+' '+$log)}
}
function ReadCandidate([string]$Path) {
    $root=[IO.Path]::GetFullPath($Path).TrimEnd('\')
    $operationRoot=Split-Path -Parent $OperationDirectory
    if(!$root.StartsWith($operationRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Editor.ScriptCandidateInvalid'}
    $document=Get-Content -LiteralPath (Join-Path $root 'candidate.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if($document.schemaVersion -ne 1 -or $document.kind -ne 'SelfEngine.EditorScriptCandidate' -or $document.projectRoot -ne $ProjectRoot -or $document.configuration -ne $Configuration){throw 'Editor.ScriptCandidateInvalid'}
    $runtimeCheck=Get-Content -LiteralPath (Join-Path $root 'runtime-check.json') -Raw -Encoding UTF8|ConvertFrom-Json
    if($runtimeCheck.schemaVersion -ne 1 -or !$runtimeCheck.success -or !$runtimeCheck.privateRuntime){throw 'Editor.ScriptCandidateInvalid'}
    foreach($file in $document.files){
        $path=[IO.Path]::GetFullPath((Join-Path $root $file.path))
        if([IO.Path]::IsPathRooted($file.path) -or !$path.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Editor.ScriptCandidateInvalid'}
        if((Get-Item -LiteralPath $path).Length -ne $file.bytes -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){throw 'Editor.ScriptCandidateInvalid'}
    }
    return [pscustomobject]@{Root=$root;Document=$document}
}
function UseCandidate($Value,[string]$State,[string]$Key) {
    $status.state=$State;$status.hasCandidate=$true;$status.candidateRoot=$Value.Root;$status.candidateIdentity=$Value.Document.sourceIdentity
    $status.diagnosticKey=$Key;$status.runtimeExecutable=Join-Path $Value.Root $Value.Document.runtimeExecutable;$status.assembly=Join-Path $Value.Root $Value.Document.assembly
    # Play 使用当前候选编译时的输入身份；失败/取消时绝不把新源码说成已编译。
    $status.sourceFiles=@($Value.Document.sourceFiles)
}
try {
    if(!$OperationDirectory.StartsWith((Join-Path $ProjectRoot 'out/Editor')+'\',[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $OperationDirectory -PathType Container)){throw 'Export.PathOutsideRoot'}
    $directory=$OperationDirectory
    while($directory){
        if((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Export.PathOutsideRoot'}
        if($directory -eq $ProjectRoot){break};$directory=Split-Path -Parent $directory
    }
    $buildPath=ProjectFile 'build.json';$manifestPath=ProjectFile 'project.json'
    $build=Get-Content -LiteralPath $buildPath -Raw -Encoding UTF8|ConvertFrom-Json
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8|ConvertFrom-Json
    if($build.format -ne 'SelfEngine.Build' -or $build.version -ne 1 -or !$build.scriptsProject -or !$build.bindings){throw 'Preview.InvalidProject'}
    $scriptProject=ProjectFile $build.scriptsProject
    if([IO.Path]::GetExtension($scriptProject) -ne '.csproj'){throw 'Preview.InvalidProject'}
    [void](ProjectFile $build.bindings);[void](ProjectFile $manifest.entryScene)
    if($build.entryUi){[void](ProjectFile $build.entryUi)}
    $status.scriptsProject=[string]$build.scriptsProject
    $sourceFiles=@(SourceInputs);$sourceIdentity=HashText (ConvertTo-Json -InputObject $sourceFiles -Depth 5 -Compress)
    $status.sourceIdentity=$sourceIdentity;$status.sourceFiles=$sourceFiles
    $lease=Open-EngineBuildLease $(if($packageMode){$ProjectRoot}else{$engineRoot})
    $native=if($packageMode){Join-Path $engineRoot 'Player/EngineNative.dll'}else{Join-Path $NativeDirectory 'EngineNative.dll'}
    if(!(Test-Path -LiteralPath $native -PathType Leaf)){throw 'Preview.NativeMissing'}
    $engineInputs=@([ordered]@{path='EngineNative.dll';sha256=(Get-FileHash -LiteralPath $native -Algorithm SHA256).Hash.ToLowerInvariant()})
    if($packageMode){$package=Read-EngineEditorPackage $engineRoot;$engineInputs+=@([ordered]@{path='editor-package.json';sha256=$package.editor_package_sha256})}
    else {
        foreach($directory in @('Source/FunctionLayer/Scripting/Bootstrap','Source/FunctionLayer/Scripting/Managed')){
            $engineInputs+=@(Get-ChildItem -LiteralPath (Join-Path $engineRoot $directory) -File -Recurse | Where-Object {$_.Extension -in @('.cs','.csproj')} | Sort-Object FullName | ForEach-Object {[ordered]@{path=$_.FullName.Substring($engineRoot.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
        }
        foreach($name in @('BuildConfig/Directory.Build.props','BuildConfig/Directory.Build.targets','BuildConfig/NuGet.Config','Source/FunctionLayer/Scripting/Directory.Build.props','global.json','Scripts/Build-EditorScriptCandidate.ps1')){$engineInputs+=@([ordered]@{path=$name;sha256=(Get-FileHash -LiteralPath (Join-Path $engineRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant()})}
    }
    $engineIdentity=HashText (ConvertTo-Json -InputObject $engineInputs -Depth 5 -Compress)
    $lastPath=Join-Path (Split-Path -Parent (Split-Path -Parent $OperationDirectory)) ('last-success-'+$Configuration.ToLowerInvariant()+'.json')
    if(Test-Path -LiteralPath $lastPath){
        try{
            $last=Get-Content -LiteralPath $lastPath -Raw -Encoding UTF8|ConvertFrom-Json
            $candidate=ReadCandidate $last.candidateRoot
            if($candidate.Document.engineIdentity -ne $engineIdentity){$candidate=$null}
        }catch{$candidate=$null;Write-Output $_.Exception.Message}
    }
    if($candidate -and $candidate.Document.sourceIdentity -eq $sourceIdentity){
        UseCandidate $candidate 'ready' '';WriteJson (Join-Path $OperationDirectory 'build-status.json') $status
        $progress.phase='reused';WriteJson (Join-Path $OperationDirectory 'build-progress.json') $progress
        exit 0
    }
    $stage=Join-Path $OperationDirectory 'candidate';if(Test-Path -LiteralPath $stage){throw 'Editor.ScriptCandidateInvalid'}
    $runtime=Join-Path $stage 'Runtime';$scripts=Join-Path $stage 'Scripts'
    New-Item -ItemType Directory -Path $runtime,$scripts | Out-Null
    $precompiled=if($packageMode){Find-EnginePrecompiledScripts $engineRoot $scriptProject}else{$null}
    $dotnet=$null
    if(!$packageMode -or !$precompiled){
        $command=Get-Command dotnet.exe -ErrorAction SilentlyContinue
        if(!$command){throw 'Export.SdkMissing'};$dotnet=$command.Source
        Step 'sdk' $dotnet @('--list-sdks')
        if((Get-Content -LiteralPath (Join-Path $OperationDirectory 'sdk.log') -Raw -Encoding UTF8) -notmatch '(?m)^10\.\d+\.\d+[^\r\n]*\['){throw 'Export.SdkMissing'}
    }
    if($packageMode){Get-ChildItem -LiteralPath (Join-Path $engineRoot 'Player') | Copy-Item -Destination $runtime -Recurse -Force}
    else {
        # 每个引擎项目有独立的 obj/bin；不会写入当前已加载的默认 Bootstrap 目录。
        $props=Join-Path $OperationDirectory 'Candidate.Directory.Build.props'
        $escapedRoot=[Security.SecurityElement]::Escape($engineRoot)
        $escapedOutput=[Security.SecurityElement]::Escape((Join-Path $OperationDirectory 'managed'))
        $text='<Project><Import Project="'+$escapedRoot+'\BuildConfig\Directory.Build.props"/><PropertyGroup><BaseOutputPath>'+$escapedOutput+'\bin\$(MSBuildProjectName)\</BaseOutputPath><BaseIntermediateOutputPath>'+$escapedOutput+'\obj\$(MSBuildProjectName)\</BaseIntermediateOutputPath></PropertyGroup></Project>'
        [IO.File]::WriteAllText($props,$text,$utf8)
        $bootstrap=Join-Path $engineRoot 'Source/FunctionLayer/Scripting/Bootstrap/Bootstrap.csproj'
        $properties=@(('-p:Configuration='+$Configuration),('-p:DirectoryBuildPropsPath='+$props),'-p:UseSharedCompilation=false','-nodeReuse:false')
        $failureKey='Preview.RestoreFailed'
        Step 'restore_host' $dotnet (@('restore',$bootstrap,'--runtime','win-x64','-p:SelfContained=true','--configfile',$nugetConfig,'--packages',(Join-Path $engineRoot 'out/nuget/packages'),'--nologo')+$properties)
        $failureKey='Preview.BuildFailed'
        Step 'publish_host' $dotnet (@('publish',$bootstrap,'--runtime','win-x64','--self-contained','true','--output',$runtime,'-p:PublishSingleFile=false','-p:PublishTrimmed=false','--no-restore','--nologo')+$properties)
        Copy-Item -LiteralPath $native -Destination (Join-Path $runtime 'EngineNative.dll')
        Copy-EngineRendererResources -NativeDirectory $NativeDirectory -Destination (Join-Path $runtime 'Resources')
        Copy-Item -LiteralPath (Join-Path $engineRoot 'Resources/Localization') -Destination (Join-Path $runtime 'Resources') -Recurse -Force
    }
    # 不加载原生引擎、不创建窗口；发布图和真实加载均通过后才能成为可用候选。
    foreach($name in @('System.IO.Pipelines.dll','System.Text.Json.dll','coreclr.dll','hostfxr.dll','hostpolicy.dll')){
        if(!(Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Leaf)){throw ('Preview.BuildFailed runtime missing '+$name)}
    }
    $runtimeConfig=Get-Content -LiteralPath (Join-Path $runtime 'SelfEngine.Bootstrap.runtimeconfig.json') -Raw -Encoding UTF8|ConvertFrom-Json
    if(!$runtimeConfig.runtimeOptions.includedFrameworks -or $runtimeConfig.runtimeOptions.framework){throw 'Preview.BuildFailed runtime must be self-contained'}
    if(!$packageMode){
        $runtimeVersion=@($runtimeConfig.runtimeOptions.includedFrameworks|Where-Object {$_.name -eq 'Microsoft.NETCore.App'})[0].version
        $runtimePackage=Join-Path $engineRoot ('out/nuget/packages/microsoft.netcore.app.runtime.win-x64/'+$runtimeVersion)
        $notices=Join-Path $runtime 'ThirdPartyNotices/DotNet';New-Item -ItemType Directory -Path $notices -Force|Out-Null
        foreach($name in @('LICENSE.TXT','THIRD-PARTY-NOTICES.TXT')){Copy-Item -LiteralPath (Join-Path $runtimePackage $name) -Destination (Join-Path $notices $name)}
    }
    $runtimeReport=Join-Path $stage 'runtime-check.json'
    Step 'verify_runtime' (Join-Path $runtime 'SelfEngine.Bootstrap.exe') @('--project-root',$ProjectRoot,'--verify-runtime',$runtimeReport)
    $verified=Get-Content -LiteralPath $runtimeReport -Raw -Encoding UTF8|ConvertFrom-Json
    if($verified.schemaVersion -ne 1 -or !$verified.success -or !$verified.privateRuntime){throw 'Preview.BuildFailed runtime verification failed'}
    if($precompiled){Get-ChildItem -LiteralPath $precompiled.Directory|Copy-Item -Destination $scripts -Recurse -Force;$assemblyName=$precompiled.Assembly}
    else {
        # 引擎引用改用本候选的 API DLL；与便携包导出使用同一 targets 适配。
        $sdkRoot=if($packageMode){$engineRoot}else{$OperationDirectory}
        if(!$packageMode){
            New-Item -ItemType Directory -Path (Join-Path $sdkRoot 'SDK/Managed') -Force|Out-Null
            Copy-Item -LiteralPath (Join-Path $runtime 'SelfEngine.Managed.dll') -Destination (Join-Path $sdkRoot 'SDK/Managed/SelfEngine.Managed.dll')
            $targets=@'
<Project><ItemGroup>
  <ProjectReference Remove="$(SelfEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj;$(SelfEngineRoot)\Source\FunctionLayer\Scripting\Managed\Engine.Managed.csproj"/>
  <Reference Remove="SelfEngine.Managed"/>
  <Reference Include="SelfEngine.Managed"><HintPath>$(MSBuildThisFileDirectory)/Managed/SelfEngine.Managed.dll</HintPath><Private>false</Private></Reference>
</ItemGroup></Project>
'@
            [IO.File]::WriteAllText((Join-Path $sdkRoot 'SDK/SelfEngine.Scripts.targets'),$targets,$utf8)
        }
        $properties=Get-EnginePackageScriptProperties $sdkRoot (Join-Path $OperationDirectory 'script-obj')
        if(!$packageMode){$properties[0]='-p:SelfEngineRoot='+$engineRoot}
        # 多个用户 ProjectReference 不能共用一个 obj/project.assets.json。
        # 保留每个项目原有 Directory.Build.props，再为各项目分配独立中间目录。
        $scriptProps=Join-Path $OperationDirectory 'Candidate.Script.Directory.Build.props'
        $isolatedObj=[Security.SecurityElement]::Escape((Join-Path $OperationDirectory 'script-obj'))
        $scriptPropsText=@'
<Project>
  <PropertyGroup><_SelfEngineOriginalProps>$([MSBuild]::GetPathOfFileAbove('Directory.Build.props', '$(MSBuildProjectDirectory)'))</_SelfEngineOriginalProps></PropertyGroup>
  <Import Project="$(_SelfEngineOriginalProps)" Condition="'$(_SelfEngineOriginalProps)' != ''"/>
  <PropertyGroup><BaseIntermediateOutputPath>__OBJECT_ROOT__\$(MSBuildProjectName)\</BaseIntermediateOutputPath><MSBuildProjectExtensionsPath>__OBJECT_ROOT__\$(MSBuildProjectName)\</MSBuildProjectExtensionsPath><DefaultItemExcludes>$(DefaultItemExcludes);**\bin\**;**\obj\**</DefaultItemExcludes></PropertyGroup>
</Project>
'@
        [IO.File]::WriteAllText($scriptProps,$scriptPropsText.Replace('__OBJECT_ROOT__',$isolatedObj),$utf8)
        $properties=@($properties|Where-Object {$_ -notmatch '^-p:(BaseIntermediateOutputPath|MSBuildProjectExtensionsPath)='})
        $properties+=@('-p:DirectoryBuildPropsPath='+$scriptProps)
        $properties+=@('-p:UseSharedCompilation=false','-nodeReuse:false')
        Step 'script_name' $dotnet (@('msbuild',$scriptProject,'-nologo','-getProperty:AssemblyName',('-p:Configuration='+$Configuration))+$properties)
        $assemblyName=(Get-Content -LiteralPath (Join-Path $OperationDirectory 'script_name.log') -Raw -Encoding UTF8).Trim()
        if(!$assemblyName -or $assemblyName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or $assemblyName -in @('.','..')){throw 'Preview.InvalidProject'}
        $failureKey='Preview.RestoreFailed'
        Step 'restore_scripts' $dotnet (@('restore',$scriptProject,'--runtime','win-x64','--configfile',$nugetConfig,'--packages',(Join-Path $ProjectRoot 'out/nuget/packages'),('-p:Configuration='+$Configuration),'-p:SelfContained=false','--nologo')+$properties)
        $failureKey='Preview.BuildFailed'
        Step 'build_scripts' $dotnet (@('publish',$scriptProject,'--configuration',$Configuration,'--runtime','win-x64','--self-contained','false','--output',$scripts,'-p:GenerateDependencyFile=true','--no-restore','--nologo')+$properties)
    }
    $assemblyRelative='Scripts/'+$assemblyName+'.dll';$assembly=Join-Path $stage $assemblyRelative
    if(!(Test-Path -LiteralPath $assembly) -or !(Test-Path -LiteralPath (Join-Path $scripts ($assemblyName+'.deps.json')))){throw 'Editor.ScriptCandidateInvalid'}
    # 只装载并验证类型目录，不调用脚本生命周期，也不要求旧字段已迁移为新 schema。
    Step 'inspect_scripts' (Join-Path $runtime 'SelfEngine.Bootstrap.exe') @('--project-root',$ProjectRoot,'--project',$assembly,'--inspect-scripts',(Join-Path $stage 'script-types.json'),'--locale',$Locale)
    if((HashText (ConvertTo-Json -InputObject @(SourceInputs) -Depth 5 -Compress)) -ne $sourceIdentity){throw 'Export.SourceChanged'}
    foreach($file in $engineInputs){
        $path=if($file.path -eq 'EngineNative.dll'){$native}else{Join-Path $engineRoot $file.path}
        if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){throw ('Export.SourceChanged '+$file.path)}
    }
    $files=@(Get-ChildItem -LiteralPath $stage -File -Recurse|Sort-Object FullName|ForEach-Object {[ordered]@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
    $document=[ordered]@{schemaVersion=1;kind='SelfEngine.EditorScriptCandidate';projectRoot=$ProjectRoot;configuration=$Configuration;sourceIdentity=$sourceIdentity;engineIdentity=$engineIdentity;sourceFiles=$sourceFiles;buildSettings=$build;projectSettings=$manifest;runtimeExecutable='Runtime/SelfEngine.Bootstrap.exe';assembly=$assemblyRelative;files=$files}
    WriteJson (Join-Path $stage 'candidate.json') $document
    $candidate=ReadCandidate $stage
    UseCandidate $candidate 'ready' ''
    WriteJson (Join-Path $OperationDirectory 'build-status.json') $status
    WriteJson $lastPath ([ordered]@{schemaVersion=1;candidateRoot=$stage})
    $progress.phase='ready';WriteJson (Join-Path $OperationDirectory 'build-progress.json') $progress
    exit 0
}catch{
    $key=($_.Exception.Message -split ' ')[0]
    if($key -notmatch '^(Preview|Export|Editor)\.'){$key='Preview.BuildFailed'}
    $status.diagnosticKey=$key
    if($candidate){UseCandidate $candidate 'stale' $key}
    Write-Output ($_.Exception.Message+"`n"+$_.InvocationInfo.PositionMessage)
    if(Test-Path -LiteralPath $OperationDirectory -PathType Container){WriteJson (Join-Path $OperationDirectory 'build-status.json') $status}
    exit 1
}finally{if($lease){$lease.Stream.Dispose()}}
