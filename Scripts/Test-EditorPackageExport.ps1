param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$Evidence,
    [switch]$SkipScriptCompilation
)
$ErrorActionPreference='Stop'
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'SourceIdentity.ps1')
. (Join-Path $PSScriptRoot 'EditorPackageRuntime.ps1')
$package=(Resolve-Path -LiteralPath $Package).Path
$evidence=[IO.Path]::GetFullPath($Evidence)
if(Test-Path -LiteralPath $evidence){throw 'Use a new package-export evidence directory.'}
New-Item -ItemType Directory -Path $evidence | Out-Null
$result=[ordered]@{schema=1;success=$false;package=$package;checks=@();script_compilation_tested=$false;clean_machine_tested=$false;error=''}
function SaveResult {Write-EngineJson (Join-Path $evidence 'package-export-verify.json') $result}
function Check([string]$name,[bool]$passed,[string]$detail=''){
    $result.checks+=@([ordered]@{name=$name;passed=$passed;detail=$detail});SaveResult
    if(!$passed){throw ('Package export check failed: '+$name+' '+$detail)}
}
function Run([string]$name,[string]$program,[string[]]$arguments){
    Push-Location -LiteralPath $env:TEMP
    try{return Invoke-EngineProgram $program $arguments (Join-Path $evidence ($name+'.log'))}
    finally{Pop-Location}
}
function Export([string]$name,[string]$projectRoot,[string]$outputName){
    return Run $name $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $relocated 'Scripts/Export-Game.ps1'),
        '-Project',(Join-Path $projectRoot 'project.json'),'-Name',$outputName,'-EvidenceDirectory',(Join-Path $projectRoot ('out/export-check/'+$name)),'-Locale','en-US')
}
$previousPath=$env:PATH
$previousLocation=(Get-Location).Path
try {
    $identity=Read-EngineEditorPackage $package
    # 搬迁到带中文和空格的独立目录；实际执行器、SDK、Player 全部来自这份副本。
    $relocated=Join-Path $evidence 'relocated/编辑器 Package'
    New-Item -ItemType Directory -Path $relocated -Force | Out-Null
    Get-ChildItem -LiteralPath $package -Force | Copy-Item -Destination $relocated -Recurse -Force
    $relocatedIdentity=Read-EngineEditorPackage $relocated
    Check 'relocated_identity' ($identity.editor_package_sha256 -eq $relocatedIdentity.editor_package_sha256)
    $powershell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $tool=Join-Path $relocated 'Tools/ProjectTool.exe'
    $sample=Join-Path $evidence 'projects/示例 Game'
    $blank=Join-Path $evidence 'projects/空白 Scene'
    New-Item -ItemType Directory -Path (Split-Path -Parent $sample) -Force | Out-Null
    $env:PATH=$env:SystemRoot+'\System32;'+$env:SystemRoot
    Check 'sdk_not_on_path' ($null -eq (Get-Command dotnet.exe -ErrorAction SilentlyContinue))
    Check 'create_sample' ((Run 'create-sample' $tool @('create',(Join-Path $relocated 'Resources/Templates/EmptyGame'),$sample)) -eq 0)
    Check 'create_blank' ((Run 'create-blank' $tool @('create',(Join-Path $relocated 'Resources/Templates/BlankScene'),$blank)) -eq 0)
    Check 'precompiled_inputs' ($null -ne (Find-EnginePrecompiledScripts $relocated (Join-Path $sample 'Scripts/EmptyGame.Scripts.csproj')))
    Check 'sample_export_without_sdk' ((Export 'sample-no-sdk' $sample 'SampleNoSdk') -eq 0)
    $sampleReport=Get-Content -LiteralPath (Join-Path $sample 'out/export-check/sample-no-sdk/export-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Check 'precompiled_selected' (@($sampleReport.steps|Where-Object mode -eq 'verified_precompiled_template').Count -eq 1)
    Check 'source_mode' ($sampleReport.source_mode -eq 'editor_package')
    $game=Join-Path $sample 'out/packages/SampleNoSdk'
    Check 'game_headless_outside_cwd' ((Run 'game-headless' $powershell @('-NoProfile','-File',(Join-Path $game 'Start-Game.ps1'),'-Headless','-Frames','120','-Locale','en-US')) -eq 0)
    Check 'blank_export_without_sdk' ((Export 'blank-no-sdk' $blank 'BlankNoSdk') -eq 0)
    $gameManifest=Join-Path $game 'package-manifest.json'
    $gameHash=(Get-FileHash -LiteralPath $gameManifest -Algorithm SHA256).Hash
    Check 'same_name_refused' ((Export 'same-name' $sample 'SampleNoSdk') -ne 0)
    Check 'old_package_unchanged' ($gameHash -eq (Get-FileHash -LiteralPath $gameManifest -Algorithm SHA256).Hash)
    # 字段值随绑定文档发布，不是 C# 编译输入；无 SDK 也能保存 Inspector 字段再导出。
    $bindingsPath=Join-Path $sample 'Scripts/Main.bindings.json'
    $bindingsDocument=Get-Content -LiteralPath $bindingsPath -Raw -Encoding UTF8|ConvertFrom-Json
    $spinBinding=@($bindingsDocument.Scripts|Where-Object Type -eq 'emptygame.spin')
    Check 'bindings_fixture' ($spinBinding.Count -eq 1)
    $spinBinding[0].Fields.RadiansPerSecond.Value=2
    [IO.File]::WriteAllText($bindingsPath,($bindingsDocument|ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))
    Check 'bindings_only_precompiled_match' ($null -ne (Find-EnginePrecompiledScripts $relocated (Join-Path $sample 'Scripts/EmptyGame.Scripts.csproj')))
    Check 'bindings_only_export_without_sdk' ((Export 'bindings-no-sdk' $sample 'BindingsNoSdk') -eq 0)
    $bindingsGame=Join-Path $sample 'out/packages/BindingsNoSdk'
    $bindingsReport=Get-Content -LiteralPath (Join-Path $sample 'out/export-check/bindings-no-sdk/export-report.json') -Raw -Encoding UTF8|ConvertFrom-Json
    Check 'bindings_only_selected_precompiled' (@($bindingsReport.steps|Where-Object mode -eq 'verified_precompiled_template').Count -eq 1)
    $publishedBindings=Get-Content -LiteralPath (Join-Path $bindingsGame 'ProjectScripts/Bindings.json') -Raw -Encoding UTF8|ConvertFrom-Json
    Check 'edited_field_published' (@($publishedBindings.Scripts|Where-Object Type -eq 'emptygame.spin')[0].Fields.RadiansPerSecond.Value -eq 2)
    Check 'bindings_only_same_assembly' ((Get-FileHash -LiteralPath (Join-Path $bindingsGame 'ProjectScripts/Custom/EmptyGame.Scripts.dll') -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath (Join-Path $game 'ProjectScripts/Custom/EmptyGame.Scripts.dll') -Algorithm SHA256).Hash)
    Check 'bindings_only_game_runs_without_sdk' ((Run 'bindings-game-headless' $powershell @('-NoProfile','-File',(Join-Path $bindingsGame 'Start-Game.ps1'),'-Headless','-Frames','120','-Locale','en-US')) -eq 0)
    $spin=Join-Path $sample 'Scripts/Spin.cs'
    $code=Get-Content -LiteralPath $spin -Raw -Encoding UTF8
    if(!$code.Contains('RadiansPerSecond=1')){throw 'Template behaviour fixture changed; update the expected edit.'}
    [IO.File]::WriteAllText($spin,$code.Replace('RadiansPerSecond=1','RadiansPerSecond=2'),[Text.UTF8Encoding]::new($false))
    Check 'modified_template_not_reused' ($null -eq (Find-EnginePrecompiledScripts $relocated (Join-Path $sample 'Scripts/EmptyGame.Scripts.csproj')))
    Check 'edited_script_without_sdk_rejected' ((Export 'edited-no-sdk' $sample 'EditedMissingSdk') -ne 0)
    $missingReport=Get-Content -LiteralPath (Join-Path $sample 'out/export-check/edited-no-sdk/export-report.json') -Raw -Encoding UTF8|ConvertFrom-Json
    Check 'missing_sdk_diagnostic' ($missingReport.error -like '*Export.SdkMissing*') $missingReport.error
    Check 'failed_candidate_removed' (!(Test-Path -LiteralPath $missingReport.staging) -and !(Test-Path -LiteralPath $missingReport.output))
    if(!$SkipScriptCompilation){
        $env:PATH=$previousPath
        Check 'sdk_available_for_edited_script' ($null -ne (Get-Command dotnet.exe -ErrorAction SilentlyContinue))
        Check 'edited_script_compiled_against_package' ((Export 'edited-with-sdk' $sample 'EditedWithSdk') -eq 0)
        $editedGame=Join-Path $sample 'out/packages/EditedWithSdk'
        $editedReport=Get-Content -LiteralPath (Join-Path $sample 'out/export-check/edited-with-sdk/export-report.json') -Raw -Encoding UTF8|ConvertFrom-Json
        Check 'edited_script_not_precompiled' (@($editedReport.steps|Where-Object phase -eq 'publish_scripts').Count -eq 1 -and @($editedReport.steps|Where-Object mode -eq 'verified_precompiled_template').Count -eq 0)
        $originalAssembly=Join-Path $game 'ProjectScripts/Custom/EmptyGame.Scripts.dll'
        $editedAssembly=Join-Path $editedGame 'ProjectScripts/Custom/EmptyGame.Scripts.dll'
        Check 'edited_assembly_changed' ((Get-FileHash -LiteralPath $originalAssembly -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $editedAssembly -Algorithm SHA256).Hash)
        $env:PATH=$env:SystemRoot+'\System32;'+$env:SystemRoot
        Check 'edited_game_runs_without_sdk' ((Run 'edited-game-headless' $powershell @('-NoProfile','-File',(Join-Path $editedGame 'Start-Game.ps1'),'-Headless','-Frames','120','-Locale','en-US')) -eq 0)
        $result.script_compilation_tested=$true
    }
    $after=Read-EngineEditorPackage $relocated
    Check 'package_remained_unchanged' ($after.editor_package_sha256 -eq $identity.editor_package_sha256)
    $result.success=$true;SaveResult
    Write-Output ('U09 package export verified: '+$evidence)
    exit 0
}catch{
    $result.error=$_.Exception.Message;SaveResult
    Write-Error $result.error -ErrorAction Continue
    exit 1
}finally{$env:PATH=$previousPath;Set-Location -LiteralPath $previousLocation}
