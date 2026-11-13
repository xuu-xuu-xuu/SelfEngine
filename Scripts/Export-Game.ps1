param([string]$Project='Resources/Templates/EmptyGame/project.json',[string]$Name='',[string]$ScriptProject='',[string]$ScriptType='',[string]$Bindings='',[string]$BuildSettings='',[string]$EntryUi='',[ValidateSet('zh-CN','en-US')][string]$Locale='zh-CN',[string]$EvidenceDirectory='',[string]$OutputDirectory='',[switch]$SkipLease,[switch]$SkipSmoke)
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$packageMode=Test-Path -LiteralPath (Join-Path $workspace 'editor-package.json') -PathType Leaf
$nugetConfig=if($packageMode){Join-Path $workspace 'NuGet.Config'}else{Join-Path $workspace 'BuildConfig/NuGet.Config'}
if($packageMode -and $PSBoundParameters.ContainsKey('Project')){$Project=[IO.Path]::GetFullPath($Project)}
Set-Location -LiteralPath $workspace
# 用户 TEMP 可能被 ACL/安全软件拒绝；导出子进程统一使用工作区可写临时目录。
$exportTemp=Join-Path $workspace '.cache/tmp/export'
New-Item -ItemType Directory -Path $exportTemp -Force | Out-Null
$env:TEMP=$exportTemp;$env:TMP=$exportTemp
# 用户 NuGet 缓存可能被 ACL 拒绝；导出使用工作区缓存。
$nugetRoot=Join-Path $workspace '.cache/nuget'
New-Item -ItemType Directory -Path $nugetRoot -Force | Out-Null
$env:NUGET_PACKAGES=Join-Path $nugetRoot 'packages'
$env:NUGET_HTTP_CACHE_PATH=Join-Path $nugetRoot 'http-cache'
$env:DOTNET_CLI_HOME=Join-Path $nugetRoot 'home'
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'ProjectBuildLease.ps1')
. (Join-Path $PSScriptRoot 'EditorPackageRuntime.ps1')
$texts=@{}
Get-Content -LiteralPath (Join-Path $workspace ('Resources/Localization/'+$Locale+'.txt')) -Encoding UTF8 | ForEach-Object {if($_ -match '^([^#=]+)=(.*)$'){$texts[$matches[1]]=$matches[2]}}
function Text([string]$Key){if($texts.ContainsKey($Key)){return $texts[$Key]};return $Key}
function Fail([string]$Key,[string]$Detail=''){throw [InvalidOperationException]::new($Key+' '+$Detail)}
function SafeProjectFile([string]$Relative) {
    if([IO.Path]::IsPathRooted($Relative)){Fail 'Export.PathOutsideRoot' $Relative}
    $path=[IO.Path]::GetFullPath((Join-Path $projectRoot $Relative))
    if(!$path.StartsWith($projectRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){Fail 'Export.PathOutsideRoot' $Relative}
    if(!(Test-Path -LiteralPath $path -PathType Leaf)){Fail 'Export.InvalidSettings' $Relative}
    $entry=Get-Item -LiteralPath $path
    while($entry -and $entry.FullName -ne $projectRoot){if($entry.Attributes -band [IO.FileAttributes]::ReparsePoint){Fail 'Export.PathOutsideRoot' $Relative};if($entry -is [IO.DirectoryInfo]){$entry=$entry.Parent}else{$entry=$entry.Directory}}
    return $path
}
function Report {Write-EngineJson (Join-Path $case 'export-report.json') $report}
function Step([string]$Phase,[string]$Executable,[string[]]$ProgramArguments) {
    $report.phase=$Phase;Report;$log=Join-Path $case ($Phase+'.log')
    $code=Invoke-EngineProgram $Executable $ProgramArguments $log
    $report.steps+=@([ordered]@{phase=$Phase;exit_code=$code;log=[IO.Path]::GetFileName($log)});Report
    if($code -ne 0){Fail 'Export.StepFailed' ($Phase+': '+$log)}
}
$lease=$null;$report=$null;$case=$null;$stage=$null;$packages=$null;$exitCode=1
try {
    if($packageMode -and !$PSBoundParameters.ContainsKey('Project')){Fail 'Export.InvalidSettings' 'Project'}
    $manifest=(Resolve-Path -LiteralPath $Project).Path;$projectRoot=Split-Path -Parent $manifest
    if($packageMode){Set-Location -LiteralPath $projectRoot}
    if($EvidenceDirectory){
        # 包目录可只读。便携模式的证据留项目 out 或编辑器用户数据目录。
        # 开发环境 out 可能受 ACL 限制；UserDataDirectory 会回退到 .cache/Editor/User。
        $evidenceRoots=@([IO.Path]::GetFullPath((Join-Path $workspace 'out')),
            [IO.Path]::GetFullPath((Join-Path $workspace '.cache/Editor/User')))
        if($packageMode){$evidenceRoots=@([IO.Path]::GetFullPath((Join-Path $projectRoot 'out')),[IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'SelfEngine/Editor')))}
        $candidate=[IO.Path]::GetFullPath($EvidenceDirectory)
        if(!@($evidenceRoots|Where-Object {$candidate.StartsWith($_.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)}).Count){Fail 'Export.PathOutsideRoot' $EvidenceDirectory}
        $case=$candidate
    }
    else{$case=Join-Path $(if($packageMode){$projectRoot}else{$workspace}) ('out/exports/'+[guid]::NewGuid().ToString('N'))}
    New-Item -ItemType Directory -Path $case -Force | Out-Null
    $report=[ordered]@{schema=1;success=$false;phase='input_validation';project=$Project;name=$Name;staging=$null;output=$null;output_root=$null;source_mode=$(if($packageMode){'editor_package'}else{'source'});steps=@()};Report
    if(!$Name){$Name='SelfEngine-'+(Get-Date -Format 'yyyyMMdd-HHmmss')}
    if($Name -notmatch '^[A-Za-z0-9_-]+$'){Fail 'Export.InvalidSettings' 'Name'}
    $settings=Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
    $scriptOverrides=$PSBoundParameters.ContainsKey('ScriptProject') -or $PSBoundParameters.ContainsKey('ScriptType') -or $PSBoundParameters.ContainsKey('Bindings')
    if(!$BuildSettings -and !$scriptOverrides -and (Test-Path -LiteralPath (Join-Path $projectRoot 'build.json'))){$BuildSettings=Join-Path $projectRoot 'build.json'}
    $build=$null
    if($BuildSettings){
        $BuildSettings=(Resolve-Path -LiteralPath $BuildSettings).Path;$build=Get-Content -LiteralPath $BuildSettings -Raw -Encoding UTF8 | ConvertFrom-Json
        if($build.format -ne 'SelfEngine.Build' -or $build.version -ne 1){Fail 'Export.InvalidSettings' $BuildSettings}
        foreach($property in $build.PSObject.Properties.Name){if($property -notin @('format','version','scriptsProject','scriptType','bindings','entryUi')){Fail 'Export.InvalidSettings' $property}}
        if(!$ScriptProject -and $build.scriptsProject){$ScriptProject=SafeProjectFile $build.scriptsProject}
        if(!$ScriptType){$ScriptType=[string]$build.scriptType}
        if(!$Bindings -and $build.bindings){$Bindings=SafeProjectFile $build.bindings}
        if(!$PSBoundParameters.ContainsKey('EntryUi')){$EntryUi=[string]$build.entryUi}
    }
    $customScripts=![string]::IsNullOrWhiteSpace($ScriptProject)
    if($customScripts -ne (![string]::IsNullOrWhiteSpace($ScriptType)) -or ($Bindings -and !$customScripts)){Fail 'Export.InvalidSettings' 'ScriptProject/ScriptType/Bindings'}
    if($customScripts){$ScriptProject=(Resolve-Path -LiteralPath $ScriptProject).Path;if([IO.Path]::GetExtension($ScriptProject) -ne '.csproj'){Fail 'Export.InvalidSettings' $ScriptProject}}
    if($Bindings){$Bindings=(Resolve-Path -LiteralPath $Bindings).Path;$bindingDocument=Get-Content -LiteralPath $Bindings -Raw -Encoding UTF8 | ConvertFrom-Json;if($bindingDocument.Version -ne 1 -or @($bindingDocument.Scripts | Where-Object Type -eq $ScriptType).Count -eq 0){Fail 'Export.InvalidSettings' $Bindings}}
    if(!$EntryUi -and !$build -and !$PSBoundParameters.ContainsKey('EntryUi')){if(@($settings.ui).Count -gt 1){Fail 'Export.EntryUiRequired'};if(@($settings.ui).Count){$EntryUi=[string]$settings.ui[0]}}
    if($EntryUi){[void](SafeProjectFile $EntryUi);if($EntryUi.Replace('\','/') -notin @($settings.ui | ForEach-Object {$_.Replace('\','/')})){Fail 'Export.InvalidSettings' 'entryUi'}}
    $packages=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $(if($packageMode){$projectRoot}else{$workspace}) 'out/packages'}
    if($packageMode -and ([IO.Path]::GetFullPath($packages).TrimEnd('\') -eq $workspace -or [IO.Path]::GetFullPath($packages).StartsWith($workspace.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase))){Fail 'Export.PathOutsideRoot' $packages}
    try{
        New-Item -ItemType Directory -Path $packages -Force | Out-Null
        # 只建父目录不足以证明可写（目录可能已存在）；用探针文件检测。
        $probe=Join-Path $packages ('.write-probe-'+[guid]::NewGuid().ToString('N'))
        Set-Content -LiteralPath $probe -Value 'probe' -Encoding ascii
        Remove-Item -LiteralPath $probe -Force
    }
    catch{
        if($OutputDirectory){throw}
        # 开发环境 out 可能只读；默认输出回退到工作区可写缓存。
        $packages=Join-Path $workspace '.cache/packages'
        New-Item -ItemType Directory -Path $packages -Force | Out-Null
    }
    $final=[IO.Path]::GetFullPath((Join-Path $packages $Name));if(Test-Path -LiteralPath $final){Fail 'Export.OutputExists' $final}
    $stage=[IO.Path]::GetFullPath((Join-Path $packages ('.'+$Name+'.stage-'+[guid]::NewGuid().ToString('N'))))
    foreach($target in @($stage,$final)){if(!$target.StartsWith([IO.Path]::GetFullPath($packages).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){Fail 'Export.PathOutsideRoot' $target}}
    # 编辑器内导出显式 SkipLease（candidate 自身持有预览锁），并只写唯一 managed 输出；
    # 命令行导出仍必须获得租约。
    $lease=$null
    if(!$SkipLease){
        for($attempt=0;$attempt -lt 20 -and !$lease;++$attempt){
            try{$lease=Open-EngineBuildLease $(if($packageMode){$projectRoot}else{$workspace})}
            catch{Start-Sleep -Milliseconds 100}
        }
        if(!$lease){throw [IO.IOException]::new('Preview.Busy')}
    }
    $report.phase='content_validation';$report.project=$manifest;$report.name=$Name;$report.staging=$stage;$report.output=$final;$report.output_root=[IO.Path]::GetFullPath($packages);Report
    $source=if($packageMode){Read-EngineEditorPackage $workspace}else{Get-EngineSourceIdentity $workspace};Write-EngineJson (Join-Path $case 'source-inputs.json') $source
    $native=if($packageMode){Join-Path $workspace 'Tools'}else{Join-Path $workspace 'out/build/windows-x64-release'}
    $nativeFiles=if($packageMode){@('ProjectTool.exe')}else{@('ProjectTool.exe','EngineNative.dll')}
    foreach($file in $nativeFiles){if(!(Test-Path -LiteralPath (Join-Path $native $file))){Fail 'Export.NativeMissing' $file}}
    $dependencyFile=Join-Path $case 'dependencies.json';Step 'content_validation' (Join-Path $native 'ProjectTool.exe') @('dependencies',$manifest,$dependencyFile)
    $powershell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $runtimeOutput=if($packageMode){Join-Path $workspace 'Player'}else{Join-Path $case 'Runtime'}
    if(!$packageMode){Step 'publish_runtime' $powershell @('-NoProfile','-File',(Join-Path $PSScriptRoot 'Publish-Managed.ps1'),'-Configuration','Release','-OutputDirectory',$runtimeOutput,'-ManagedOutputRoot',(Join-Path $case 'managed'))}
    else{$report.phase='publish_runtime';$report.steps+=@([ordered]@{phase='publish_runtime';exit_code=0;input='Player';mode='prebuilt'});Report}
    New-Item -ItemType Directory -Path $stage | Out-Null
    Get-ChildItem -LiteralPath $runtimeOutput | Copy-Item -Destination $stage -Recurse -Force
    foreach($file in $(if($packageMode){@('ProjectTool.exe')}else{@('ProjectTool.exe')})){Copy-Item -LiteralPath (Join-Path $native $file) -Destination $stage}
    $scriptAssembly='ProjectScripts/SelfEngine.ProjectScripts.dll';$scriptBindings=''
    if($customScripts){
        $scriptInputs=@(Get-EngineScriptInputs $ScriptProject)
        $precompiled=if($packageMode){Find-EnginePrecompiledScripts $workspace $ScriptProject}else{$null}
        $custom=Join-Path $stage 'ProjectScripts/Custom'
        if($precompiled){
            New-Item -ItemType Directory -Path $custom -Force | Out-Null
            Get-ChildItem -LiteralPath $precompiled.Directory | Copy-Item -Destination $custom -Recurse -Force
            $assemblyName=$precompiled.Assembly
            $report.phase='publish_scripts';$report.steps+=@([ordered]@{phase='publish_scripts';exit_code=0;mode='verified_precompiled_template'});Report
        }else{
        $command=Get-Command dotnet.exe -ErrorAction SilentlyContinue
        if(!$command){Fail 'Export.SdkMissing'}
        $dotnet=$command.Source
        Step 'script_sdk' $dotnet @('--list-sdks')
        if((Get-Content -LiteralPath (Join-Path $case 'script_sdk.log') -Raw -Encoding UTF8) -notmatch '(?m)^10\.\d+\.\d+[^\r\n]*\['){Fail 'Export.SdkMissing'}
        $scriptProperties=if($packageMode){Get-EnginePackageScriptProperties $workspace (Join-Path $case 'script-obj')}else{@('-p:SelfEngineRoot='+$workspace)}
        Step 'script_name' $dotnet (@('msbuild',$ScriptProject,'-nologo','-getProperty:AssemblyName','-p:Configuration=Release','-p:RuntimeIdentifier=win-x64')+$scriptProperties)
        $assemblyName=(Get-Content -LiteralPath (Join-Path $case 'script_name.log') -Raw -Encoding UTF8).Trim()
        if(!$assemblyName -or $assemblyName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or $assemblyName -in @('.','..')){Fail 'Export.InvalidSettings' 'AssemblyName'}
        $nugetRoot=Join-Path $(if($packageMode){$projectRoot}else{$workspace}) 'out/nuget/packages'
        Step 'restore_scripts' $dotnet (@('restore',$ScriptProject,'--runtime','win-x64','--configfile',$nugetConfig,'--packages',$nugetRoot,'-p:Configuration=Release','-p:SelfContained=false','--nologo')+$scriptProperties)
        Step 'publish_scripts' $dotnet (@('publish',$ScriptProject,'--configuration','Release','--runtime','win-x64','--self-contained','false','--output',$custom,'-p:PublishSingleFile=false','-p:PublishTrimmed=false','-p:GenerateDependencyFile=true','--no-restore','--nologo')+$scriptProperties)
        }
        $scriptAssembly='ProjectScripts/Custom/'+$assemblyName+'.dll'
        if(!(Test-Path -LiteralPath (Join-Path $stage $scriptAssembly)) -or !(Test-Path -LiteralPath (Join-Path $custom ($assemblyName+'.deps.json')))){Fail 'Export.StepFailed' 'script output'}
        if($Bindings){$scriptBindings='ProjectScripts/Bindings.json';Copy-Item -LiteralPath $Bindings -Destination (Join-Path $stage $scriptBindings)}
    }else{$ScriptType='sample.project_mover'}
    $copiedProject=Join-Path $stage 'Project';$dependencies=Get-Content -LiteralPath $dependencyFile -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($file in $dependencies.files){
        $original=SafeProjectFile $file.path;$target=[IO.Path]::GetFullPath((Join-Path $copiedProject $file.path))
        if(!$target.StartsWith($copiedProject.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){Fail 'Export.PathOutsideRoot' $file.path}
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null;Copy-Item -LiteralPath $original -Destination $target
        if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){Fail 'Export.SourceChanged' $file.path}
    }
    # 依赖清单只列文件；空的内容根目录不会出现在其中，但导出预检与游戏启动都要求它存在。
    if([IO.Path]::IsPathRooted($settings.contentRoot)){Fail 'Export.PathOutsideRoot' $settings.contentRoot}
    $contentDirectory=[IO.Path]::GetFullPath((Join-Path $copiedProject $settings.contentRoot))
    if(!$contentDirectory.StartsWith($copiedProject.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){Fail 'Export.PathOutsideRoot' $settings.contentRoot}
    if(!(Test-Path -LiteralPath $contentDirectory)){New-Item -ItemType Directory -Path $contentDirectory -Force | Out-Null}
    $cooked=@();$cache=Join-Path $copiedProject '.cache/assets';New-Item -ItemType Directory -Path $cache -Force | Out-Null
    foreach($asset in @($settings.assets | Where-Object type -eq 'model')){foreach($suffix in @('.model.json','.manifest.json')){
        $path=Join-Path (Join-Path $projectRoot '.cache/assets') ($asset.id+$suffix);if(!(Test-Path -LiteralPath $path)){Fail 'Export.CookFailed' $asset.id};Copy-Item -LiteralPath $path -Destination $cache;$cooked+=@([ordered]@{path='Project/.cache/assets/'+$asset.id+$suffix;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()})
    }}
    Copy-Item -LiteralPath $dependencyFile -Destination (Join-Path $stage 'dependencies.json');Write-EngineJson (Join-Path $stage 'processed-assets.json') ([ordered]@{schema=1;model_format='validated StaticModel JSON/cache manifest';texture_format='validated source image';material_format='validated material JSON';files=$cooked})
    if($packageMode){$crt=Join-Path $workspace 'Runtime'}else{
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe';$vs=(& $vswhere -version '[17.0,18.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim();if($LASTEXITCODE -ne 0 -or !$vs){Fail 'Export.NativeMissing' 'MSVC redist'}
    $crt=$null;foreach($version in (Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Redist/MSVC') -Directory | Where-Object Name -match '^\d+\.\d+\.\d+$' | Sort-Object {[version]$_.Name} -Descending)){$candidate=Join-Path $version.FullName 'x64/Microsoft.VC143.CRT';if(Test-Path -LiteralPath $candidate){$crt=$candidate;break}}
    if(!$crt){Fail 'Export.NativeMissing' 'MSVC redist'}
    }
    $crtFiles=@(Get-ChildItem -LiteralPath $crt -Filter '*.dll' | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $stage;[ordered]@{path=$_.Name;version=$_.VersionInfo.FileVersion;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
    foreach($license in @(@('ufbx','LICENSE'),@('cgltf','LICENSE'),@('nlohmann','LICENSE.MIT'),@('imgui','LICENSE.txt'))){$directory=Join-Path $stage ('ThirdParty/'+$license[0]);New-Item -ItemType Directory -Path $directory -Force | Out-Null;Copy-Item -LiteralPath (Join-Path $workspace ('ThirdParty/'+$license[0]+'/'+$license[1])) -Destination $directory}
    if(Test-Path -LiteralPath (Join-Path $workspace 'ThirdParty/README.md') -PathType Leaf){
        Copy-Item -LiteralPath (Join-Path $workspace 'ThirdParty/README.md') -Destination (Join-Path $stage 'ThirdParty/README.md')
    }
    Write-EngineJson (Join-Path $stage 'runtime-dependencies.json') ([ordered]@{schema=1;architecture='win-x64';dotnet='self-contained multi-file publish; see SelfEngine.Bootstrap.runtimeconfig.json';sdk_required=$false;crt=$crtFiles;system='Windows 10/11 x64, installed D3D12-capable GPU driver, Windows WIC/DirectWrite/system fonts';clean_machine_tested=$false})
    Write-EngineJson (Join-Path $stage 'script-dependencies.json') ([ordered]@{schema=1;assembly=$scriptAssembly;scriptType=$ScriptType;files=@(Get-ChildItem -LiteralPath (Join-Path $stage 'ProjectScripts') -File -Recurse | Sort-Object FullName | ForEach-Object {[ordered]@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})})
    Write-EngineJson (Join-Path $stage 'launch.json') ([ordered]@{schema=1;scene='Project/'+$settings.entryScene;ui=$(if($EntryUi){'Project/'+$EntryUi}else{''});manifest='Project/'+[IO.Path]::GetFileName($manifest);assetRoot='Project/'+$settings.contentRoot;assetCache='Project/.cache/assets';scriptAssembly=$scriptAssembly;scriptType=$ScriptType;bindings=$scriptBindings;menu=!$customScripts})
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Start-ExportedGame.ps1') -Destination (Join-Path $stage 'Start-Game.ps1')
    $preflight=@('--project-root',$copiedProject,'--scene',(Join-Path $copiedProject $settings.entryScene),'--asset-root',(Join-Path $copiedProject $settings.contentRoot),'--asset-cache',$cache,'--project',(Join-Path $stage $scriptAssembly),'--script',$ScriptType,'--locale',$Locale,'--validate-bindings')
    if($scriptBindings){$preflight+=@('--bindings',(Join-Path $stage $scriptBindings))};if($EntryUi){$preflight+=@('--ui',(Join-Path $copiedProject $EntryUi))};if(!$customScripts){$preflight+='--menu'}
    Step 'field_preflight' (Join-Path $stage 'SelfEngine.Bootstrap.exe') $preflight
    Step 'packaged_content' (Join-Path $stage 'ProjectTool.exe') @('validate',(Join-Path $copiedProject ([IO.Path]::GetFileName($manifest))))
    if(!$SkipSmoke){
        Step 'headless_smoke' $powershell @('-NoProfile','-File',(Join-Path $stage 'Start-Game.ps1'),'-Headless','-Frames','120','-Locale',$Locale)
        $capture=Join-Path $case 'game-window.ppm';Step 'window_smoke' $powershell @('-NoProfile','-File',(Join-Path $stage 'Start-Game.ps1'),'-Hidden','-Frames','12','-Capture',$capture,'-CaptureFrame','12','-Locale',$Locale)
        if(!(Test-Path -LiteralPath $capture)){Fail 'Export.StepFailed' 'window capture missing'}
    }
    foreach($file in $cooked){if((Get-FileHash -LiteralPath (Join-Path $stage $file.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){Fail 'Export.CookFailed' $file.path}}
    if($packageMode){if((Read-EngineEditorPackage $workspace).editor_package_sha256 -ne $source.editor_package_sha256){Fail 'Export.SourceChanged' 'editor package'}}
    elseif((Get-EngineSourceIdentity $workspace).source_sha256 -ne $source.source_sha256){Fail 'Export.SourceChanged' 'engine source'}
    if($customScripts -and (ConvertTo-Json -InputObject @(Get-EngineScriptInputs $ScriptProject) -Depth 4 -Compress) -ne (ConvertTo-Json -InputObject $scriptInputs -Depth 4 -Compress)){Fail 'Export.SourceChanged' 'script inputs'}
    $report.phase='validated';Report;$archive=Join-Path $stage 'BuildReport';New-Item -ItemType Directory -Path $archive | Out-Null;Get-ChildItem -LiteralPath $case -File | Copy-Item -Destination $archive
    if(Test-Path -LiteralPath (Join-Path $workspace 'Docs/ExportedGameReadme.md') -PathType Leaf){
        Copy-Item -LiteralPath (Join-Path $workspace 'Docs/ExportedGameReadme.md') -Destination (Join-Path $stage 'README.md')
    }
    $report.phase='manifest';Report
    $files=@(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {[ordered]@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
    [int64]$totalBytes=0;foreach($file in $files){$totalBytes+=[int64]$file.bytes}
    Write-EngineJson (Join-Path $stage 'package-manifest.json') ([ordered]@{schema=1;kind='SelfEngine.Game';validated=$true;source_sha256=$source.source_sha256;configuration='Release';clean_machine_tested=$false;files=$files;file_count=$files.Count;total_bytes=$totalBytes})
    [IO.Directory]::Move($stage,$final) # 已检查的同卷输出路径；并发创建同名目标也不会覆盖。
    $report.success=$true;$report.phase='complete';Report;Write-Output ((Text 'Export.Completed')+': '+$final);$exitCode=0
} catch {
    if($report){$report.success=$false;$report.failed_phase=$report.phase;$report.phase='failed';$report.error=$_.Exception.Message;$report.error_position=$_.InvocationInfo.PositionMessage;$report.error_stack=$_.ScriptStackTrace;Report}
    Write-Error -Message ((Text 'Export.Failed')+': '+$_.Exception.Message+$(if($case){' ['+$case+']'}else{''})) -ErrorAction Continue
} finally {
    if($exitCode -ne 0 -and $stage){
        try{Remove-EngineExportStage $stage $packages $Name;if($report){$report.staging_removed=$true;Report}}
        catch{if($report){$report.cleanup_error=$_.Exception.Message;Report}}
    }
    if($lease){$lease.Stream.Dispose()}
}
exit $exitCode
