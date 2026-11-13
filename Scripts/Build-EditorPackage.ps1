param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [string]$Name='',
    [string]$OutputDirectory='',
    [string]$SelfEngineRoot=''
)
$ErrorActionPreference='Stop'
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
$root=if($SelfEngineRoot){[IO.Path]::GetFullPath($SelfEngineRoot)}else{(Resolve-Path (Join-Path $PSScriptRoot '..')).Path}
Set-Location -LiteralPath $root
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'ProjectBuildLease.ps1')
. (Join-Path $PSScriptRoot 'EditorPackageRuntime.ps1')
if($Configuration -ne 'Release'){throw 'Export.PackageRequiresRelease'}
$config=$Configuration.ToLowerInvariant()
$native=Join-Path $root ('out/build/windows-x64-'+$config)
$executable=Join-Path $native 'FlareEditor.exe'
if(!(Test-Path -LiteralPath $executable)){throw ('FlareEditor.exe is missing; build the '+$Configuration+' preset first: '+$executable)}
if(!$Name){$Name='FlareEditor-'+(Get-Date -Format 'yyyyMMdd-HHmmss')}
if($Name -notmatch '^[A-Za-z0-9_-]+$'){throw 'Package name must use letters, digits, underscore or hyphen.'}
$outputRoot=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $root 'out/packages'}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$final=Join-Path $outputRoot $Name
if(Test-Path -LiteralPath $final){throw ('Package output already exists; choose a new name: '+$final)}
$stage=Join-Path $outputRoot ('.'+$Name+'.stage-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$evidence=Join-Path $root ('out/validation/editor-product/U09-package-'+$Name+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$report=[ordered]@{schema=1;success=$false;package=$Name;configuration=$Configuration;stage=$stage;final=$final;evidence=$evidence;steps=@();error=''}
$reportPath=Join-Path $evidence 'package-build.json'
function WriteReport {[IO.File]::WriteAllText($reportPath,($report|ConvertTo-Json -Depth 8)+"`n",[Text.UTF8Encoding]::new($false))}
function AddStep([string]$step){$report.steps+=@($step);WriteReport}
function CopyFile([string]$source,[string]$destination){New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null;Copy-Item -LiteralPath $source -Destination $destination -Force}
function CopyTree([string]$source,[string]$destination){New-Item -ItemType Directory -Force -Path $destination | Out-Null;Get-ChildItem -LiteralPath $source -Force | Copy-Item -Destination $destination -Recurse -Force}
function RunStep([string]$step,[string]$program,[string[]]$arguments){
    AddStep $step
    $code=Invoke-EngineProgram $program $arguments (Join-Path $evidence ($step+'.log'))
    if($code -ne 0){throw ('Export.StepFailed '+$step+' exit='+$code)}
}
$lease=$null
try {
    WriteReport
    $lease=Open-EngineBuildLease $root
    $sourceIdentity=Get-EngineSourceIdentity $root
    AddStep 'copy_editor'
    CopyFile $executable (Join-Path $stage 'FlareEditor.exe')
    CopyFile $executable (Join-Path $stage 'GearEditor.exe')
    CopyFile $executable (Join-Path $stage 'SelfEngineEditor.exe')
    AddStep 'copy_resources'
    CopyTree (Join-Path $native 'Resources') (Join-Path $stage 'Resources')
    AddStep 'strip_derived_caches'
    # 模板里可能留有开发机构建出的派生缓存/中间目录；便携包不带缓存与临时产物。
    $derived=@(Get-ChildItem -LiteralPath (Join-Path $stage 'Resources') -Directory -Recurse -Force | Where-Object {$_.Name -in @('.cache','bin','obj')} | Sort-Object {$_.FullName.Length} -Descending)
    foreach($directory in $derived){
        $target=[IO.Path]::GetFullPath($directory.FullName)
        if(!$target.StartsWith([IO.Path]::GetFullPath($stage).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Export.PathOutsideRoot'}
        if(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target -Recurse -Force}
    }
    AddStep 'copy_tools'
    foreach($tool in @('ProjectTool.exe','ShaderBuilder.exe')){
        $source=Join-Path $native $tool
        if(!(Test-Path -LiteralPath $source)){throw ('Export.NativeMissing '+$tool)}
        CopyFile $source (Join-Path $stage ('Tools/'+$tool))
    }
    AddStep 'copy_scripts'
    foreach($script in @('Export-Game.ps1','Export-Package.ps1','Start-ExportedGame.ps1','NativeProgram.ps1','SourceIdentity.ps1','ProjectBuildLease.ps1','EditorPackageRuntime.ps1','Build-EditorScriptCandidate.ps1','Copy-RendererResources.ps1')){
        CopyFile (Join-Path $root ('Scripts/'+$script)) (Join-Path $stage ('Scripts/'+$script))
    }
    CopyFile (Join-Path $root 'NuGet.Config') (Join-Path $stage 'NuGet.Config')
    if(Test-Path -LiteralPath (Join-Path $root 'Docs/ExportedGameReadme.md') -PathType Leaf){
        CopyFile (Join-Path $root 'Docs/ExportedGameReadme.md') (Join-Path $stage 'Docs/ExportedGameReadme.md')
    }
    $powershell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $runtimeOutput=Join-Path $evidence 'published-player'
    RunStep 'publish_player' $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $root 'Scripts/Publish-Managed.ps1'),'-Configuration','Release','-OutputDirectory',$runtimeOutput)
    CopyTree $runtimeOutput (Join-Path $stage 'Player')
    AddStep 'copy_managed_sdk'
    CopyFile (Join-Path $runtimeOutput 'SelfEngine.Managed.dll') (Join-Path $stage 'SDK/Managed/SelfEngine.Managed.dll')
    $sdkProps=@'
<Project>
  <PropertyGroup>
    <TargetFramework Condition="'$(TargetFramework)' == ''">net10.0</TargetFramework>
    <PlatformTarget>x64</PlatformTarget>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <SelfContained>false</SelfContained>
    <UseAppHost>false</UseAppHost>
  </PropertyGroup>
</Project>
'@
    $sdkTargets=@'
<Project>
  <!-- 旧源码模板与包内新模板都绑定当前包的同一 API，不重新构建引擎。 -->
  <ItemGroup>
    <ProjectReference Remove="$(SelfEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj;$(SelfEngineRoot)\Source\FunctionLayer\Scripting\Managed\Engine.Managed.csproj" />
    <Reference Remove="SelfEngine.Managed" />
    <Reference Include="SelfEngine.Managed">
      <HintPath>$(MSBuildThisFileDirectory)Managed/SelfEngine.Managed.dll</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
  <Target Name="SelfEngineValidatePackageApi" BeforeTargets="ResolveReferences">
    <Error Condition="!Exists('$(MSBuildThisFileDirectory)Managed/SelfEngine.Managed.dll')" Text="Export.EditorPackageInvalid: SelfEngine.Managed.dll" />
  </Target>
</Project>
'@
    [IO.File]::WriteAllText((Join-Path $stage 'SDK/SelfEngine.Scripts.props'),$sdkProps,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $stage 'SDK/SelfEngine.Scripts.targets'),$sdkTargets,[Text.UTF8Encoding]::new($false))
    AddStep 'prepare_package_template'
    $template=Join-Path $stage 'Resources/Templates/EmptyGame'
    $scriptProject=Join-Path $template 'Scripts/EmptyGame.Scripts.csproj'
    $xml=New-Object Xml.XmlDocument
    $xml.PreserveWhitespace=$true;$xml.Load($scriptProject)
    foreach($reference in @($xml.SelectNodes('//ProjectReference'))){[void]$reference.ParentNode.RemoveChild($reference)}
    $import=$xml.CreateElement('Import');$import.SetAttribute('Project','$(SelfEngineRoot)/SDK/SelfEngine.Scripts.props');$import.SetAttribute('Condition',"Exists('`$(SelfEngineRoot)/SDK/SelfEngine.Scripts.props')")
    [void]$xml.DocumentElement.PrependChild($import)
    $group=$xml.CreateElement('ItemGroup');$reference=$xml.CreateElement('Reference');$reference.SetAttribute('Include','SelfEngine.Managed')
    $hint=$xml.CreateElement('HintPath');$hint.InnerText='$(SelfEngineRoot)/SDK/Managed/SelfEngine.Managed.dll';[void]$reference.AppendChild($hint)
    $private=$xml.CreateElement('Private');$private.InnerText='false';[void]$reference.AppendChild($private);[void]$group.AppendChild($reference);[void]$xml.DocumentElement.AppendChild($group)
    foreach($errorNode in @($xml.SelectNodes('//Target[@Name="CheckEngineRoot"]/Error'))){$errorNode.SetAttribute('Text','Export.EditorPackageInvalid: set SelfEngineRoot to the editor package directory.')}
    $writerSettings=New-Object Xml.XmlWriterSettings;$writerSettings.Encoding=[Text.UTF8Encoding]::new($false);$writerSettings.Indent=$true
    $writer=[Xml.XmlWriter]::Create($scriptProject,$writerSettings);try{$xml.Save($writer)}finally{$writer.Dispose()}
    # 与模板已有 Directory.Build.props 配对，避免项目位置意外引入外层构建定制。
    [IO.File]::WriteAllText((Join-Path $template 'Scripts/Directory.Build.targets'),'<Project />',[Text.UTF8Encoding]::new($false))
    # 不要用 $name：PowerShell 变量不区分大小写，会覆盖上面的包名 $Name，导致清单 name 错成 Build.ps1。
    foreach($legacyName in @('Main.cpp','CMakeLists.txt','Build.ps1')){
        $legacy=Join-Path $template $legacyName
        if(Test-Path -LiteralPath $legacy){Remove-Item -LiteralPath $legacy}
    }
    [IO.File]::WriteAllText((Join-Path $template 'README.md'),"# 空游戏模板`n`n从 Flare 引擎打开 project.json，编辑后在构建与导出面板生成游戏。`n未修改的示例行为可直接使用随包预编译版本；修改 Scripts 后需要 .NET 10 SDK。`n",[Text.UTF8Encoding]::new($false))
    $dotnet=(Get-Command dotnet.exe -ErrorAction Stop).Source
    $scriptProperties=Get-EnginePackageScriptProperties $stage (Join-Path $evidence 'template-obj')
    RunStep 'restore_template_scripts' $dotnet (@('restore',$scriptProject,'--runtime','win-x64','--configfile',(Join-Path $root 'NuGet.Config'),'--packages',(Join-Path $root 'out/nuget/packages'),'-p:Configuration=Release','-p:SelfContained=false','--nologo')+$scriptProperties)
    $precompiled=Join-Path $stage 'SDK/Precompiled/EmptyGame'
    RunStep 'publish_template_scripts' $dotnet (@('publish',$scriptProject,'--configuration','Release','--runtime','win-x64','--self-contained','false','--output',(Join-Path $precompiled 'Files'),'-p:PublishSingleFile=false','-p:PublishTrimmed=false','-p:GenerateDependencyFile=true','--no-restore','--nologo')+$scriptProperties)
    foreach($required in @('EmptyGame.Scripts.dll','EmptyGame.Scripts.deps.json')){if(!(Test-Path -LiteralPath (Join-Path $precompiled ('Files/'+$required)))){throw ('Export.StepFailed '+$required)}}
    Write-EngineJson (Join-Path $precompiled 'manifest.json') ([ordered]@{schema=1;project='EmptyGame.Scripts.csproj';assembly='EmptyGame.Scripts';source_files=@(Get-EngineScriptInputs $scriptProject)})
    AddStep 'clean_template_outputs'
    # dotnet publish 会在模板 Scripts 下重建 bin/obj；包内只保留预编译产物，不携带派生缓存。
    foreach($derivedName in @('bin','obj')){
        $derivedPath=Join-Path $template ('Scripts/'+$derivedName)
        if(Test-Path -LiteralPath $derivedPath){Remove-Item -LiteralPath $derivedPath -Recurse -Force}
    }
    AddStep 'copy_licenses'
    foreach($directory in Get-ChildItem -LiteralPath (Join-Path $root 'ThirdParty') -Directory){
        foreach($license in (Get-ChildItem -LiteralPath $directory.FullName -File | Where-Object {$_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)'})){
            CopyFile $license.FullName (Join-Path $stage ('ThirdParty/'+$directory.Name+'/'+$license.Name))
        }
    }
    if(Test-Path -LiteralPath (Join-Path $root 'ThirdParty/README.md') -PathType Leaf){
        CopyFile (Join-Path $root 'ThirdParty/README.md') (Join-Path $stage 'ThirdParty/README.md')
    }
    AddStep 'copy_runtime'
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if(!(Test-Path -LiteralPath $vswhere)){throw 'vswhere.exe is missing; Visual Studio x64 build tools are required to locate the runtime.'}
    $installation=(& $vswhere -version '[17.0,18.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    $crt=$null
    foreach($version in (Get-ChildItem -LiteralPath (Join-Path $installation 'VC/Redist/MSVC') -Directory | Where-Object Name -match '^\d+\.\d+\.\d+$' | Sort-Object {[version]$_.Name} -Descending)){
        $candidate=Join-Path $version.FullName 'x64/Microsoft.VC143.CRT'
        if(Test-Path -LiteralPath $candidate){$crt=$candidate;break}
    }
    if(!$crt){throw 'MSVC x64 redistributable directory was not found.'}
    foreach($file in Get-ChildItem -LiteralPath $crt -Filter '*.dll'){
        # 原生导入由 Windows 在 main 之前解析；每个入口需要 app-local CRT。
        foreach($directory in @('','Runtime','Tools','Player')){CopyFile $file.FullName (Join-Path (Join-Path $stage $directory) $file.Name)}
    }
    CopyTree (Join-Path $stage 'Resources/Localization') (Join-Path $stage 'Tools/Resources/Localization')
    AddStep 'write_readme'
    if(Test-Path -LiteralPath (Join-Path $root 'Docs/EditorPackageReadme.md') -PathType Leaf){
        CopyFile (Join-Path $root 'Docs/EditorPackageReadme.md') (Join-Path $stage 'README.md')
    }
    $startScript=@'
param([string]$Project='',[ValidateSet('zh-CN','en-US')][string]$Language='zh-CN',[switch]$SafeMode)
$ErrorActionPreference='Stop'
$executable=Join-Path $PSScriptRoot 'FlareEditor.exe'
$arguments=@('--language',$Language)
if($Project){$arguments=@('--project',$Project)+$arguments}
if($SafeMode){$arguments+='--safe-mode'}
& $executable @arguments
exit $LASTEXITCODE
'@
    [IO.File]::WriteAllText((Join-Path $stage 'Start-Editor.ps1'),$startScript,[Text.UTF8Encoding]::new($false))
    AddStep 'check_forbidden_paths'
    $forbidden=@(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {$_.FullName -match '[\\/]out[\\/]Editor[\\/]User[\\/]|[\\/]\.git[\\/]|[\\/]\.cache[\\/]|[\\/]Source[\\/]|[\\/]Tests[\\/]'})
    if($forbidden.Count){throw ('Forbidden package path: '+$forbidden[0].FullName)}
    if(Test-Path -LiteralPath (Join-Path $stage 'CMakeLists.txt')){throw 'Package must not contain the engine source tree.'}
    $absolute=@(Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object {$_.Extension -in '.txt','.ps1','.json','.md','.cfg'} | Select-String -Pattern ([regex]::Escape($root)) -List)
    if($absolute.Count){throw ('Package text contains an absolute workspace path: '+$absolute[0].Path)}
    AddStep 'startup_check'
    $start=Start-Process -FilePath (Join-Path $stage 'FlareEditor.exe') -ArgumentList @('--smoke-launcher','400','--no-ui-errors') -WorkingDirectory $env:TEMP -PassThru -Wait -WindowStyle Hidden
    if($start.ExitCode -ne 0){throw ('Package startup check failed with exit code '+$start.ExitCode)}
    AddStep 'write_manifest'
    $finalSource=Get-EngineSourceIdentity $root
    if($finalSource.source_sha256 -ne $sourceIdentity.source_sha256){throw 'Export.SourceChanged'}
    $files=@(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    [int64]$totalBytes=0;foreach($file in $files){$totalBytes+=[int64]$file.bytes}
    $manifest=[ordered]@{
        schema=1;kind='SelfEngine.EditorPackage';name=$Name;version=(Get-Date -Format 'yyyy.MM.dd');
        configuration=$Configuration;git_head=$sourceIdentity.git_head;source_dirty=$sourceIdentity.dirty;source_sha256=$sourceIdentity.source_sha256;
        created_utc=(Get-Date).ToUniversalTime().ToString('o');clean_machine_tested=$false;package_export_tested=$false;export_contract=1;
        runtime=@{player='Player';tools='Tools';managed_api='SDK/Managed';script_targets='SDK/SelfEngine.Scripts.targets';dotnet_sdk='10.0';precompiled_template='SDK/Precompiled/EmptyGame'};
        files=$files;file_count=$files.Count;total_bytes=$totalBytes
    }
    Write-EngineJson (Join-Path $stage 'editor-package.json') $manifest
    AddStep 'verify_manifest'
    $listed=@{};$hashes=@{};foreach($item in $manifest.files){$listed[$item.path]=$true;$hashes[$item.path]=$item.sha256}
    foreach($file in Get-ChildItem -LiteralPath $stage -File -Recurse){
        $relative=$file.FullName.Substring($stage.Length+1).Replace('\','/')
        if($relative -eq 'editor-package.json'){continue}
        if(!$listed.ContainsKey($relative)){throw ('Manifest misses file: '+$relative)}
    }
    foreach($path in $listed.Keys){
        $target=Join-Path $stage ($path.Replace('/','\'))
        if(!(Test-Path -LiteralPath $target)){throw ('Manifest lists a missing file: '+$path)}
        if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hashes[$path]){throw ('Manifest hash mismatch: '+$path)}
    }
    AddStep 'publish'
    [IO.Directory]::Move($stage,$final)
    $report.success=$true;$report.final=$final;$report.file_count=$manifest.file_count;$report.total_bytes=$manifest.total_bytes
    WriteReport
    Write-Output ('Editor package created: '+$final)
    Write-Output ('files='+$manifest.file_count+' bytes='+$manifest.total_bytes+' manifest='+(Join-Path $final 'editor-package.json'))
    exit 0
}
catch {
    $report.error=$_.Exception.Message
    WriteReport
    try{Remove-EngineExportStage $stage $outputRoot $Name}catch{Write-Warning $_.Exception.Message}
    Write-Error ('Editor package build failed: '+$_.Exception.Message) -ErrorAction Continue
    exit 1
}
finally{if($lease){$lease.Stream.Dispose()}}
