param([string]$OutputDirectory='')
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
$ErrorActionPreference='Stop'
$repository=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path;Set-Location -LiteralPath $repository
. (Join-Path $repository 'Scripts/NativeProgram.ps1')
. (Join-Path $repository 'Scripts/SourceIdentity.ps1')
function Check([bool]$Value,[string]$Message){if(!$Value){throw $Message}}
if(!$OutputDirectory){$OutputDirectory=Join-Path $repository ('out/validation/package-export/'+[guid]::NewGuid().ToString('N'))}
$root=[IO.Path]::GetFullPath($OutputDirectory);Check (!(Test-Path -LiteralPath $root)) 'Use a new export test directory.';New-Item -ItemType Directory -Path $root -Force | Out-Null
$native=Join-Path $repository 'out/build/windows-x64-release';$powershell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe';$managed=Join-Path $repository 'out/managed/ManagedTests/Release/net10.0/SelfEngine.ManagedTests.dll'
$project=Join-Path $root '新游戏 Project'
Check ((Invoke-EngineProgram (Join-Path $native 'ProjectTool.exe') @('create',(Join-Path $repository 'Resources/Templates/EmptyGame'),$project) (Join-Path $root 'create.log')) -eq 0) 'Template create failed.'
$preview=@('-NoProfile','-File',(Join-Path $repository 'Scripts/Run-ProjectPreview.ps1'),'-ProjectRoot',$project,'-ScriptsProject','Scripts/EmptyGame.Scripts.csproj','-Scene','Scenes/Main.scene.json','-Bindings','Scripts/Main.bindings.json','-Configuration','Release','-Mode','Editor','-Play','-Hidden','-Frames','6','-Locale','en-US')
Check ((Invoke-EngineProgram $powershell $preview (Join-Path $root 'preview.log')) -eq 0) 'New template preview failed.'
$previewCase=Get-ChildItem -LiteralPath (Join-Path $project 'out/Preview') -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$previewReport=Get-Content -LiteralPath (Join-Path $previewCase.FullName 'preview.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($previewReport.success) 'New template preview incomplete.'
$dotnet=(Get-Command dotnet.exe).Source
Check ((Invoke-EngineProgram $dotnet @($managed,'--prepare-export',$project,$previewReport.assembly) (Join-Path $root 'authoring.log')) -eq 0) 'Template authoring/save failed.'
$name='T65-'+[guid]::NewGuid().ToString('N');$package=Join-Path $repository ('out/packages/'+$name)
function Export([string]$ProjectDirectory,[string]$PackageName,[string]$Log) {
    return Invoke-EngineProgram $powershell @('-NoProfile','-File',(Join-Path $repository 'Scripts/Export-Package.ps1'),'-Project',(Join-Path $ProjectDirectory 'project.json'),'-Name',$PackageName,'-Locale','en-US') $Log
}
function VerifyManifest([string]$Directory) {
    $manifest=Get-Content -LiteralPath (Join-Path $Directory 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Check ($manifest.validated -and !$manifest.clean_machine_tested -and $manifest.file_count -eq $manifest.files.Count -and $manifest.file_count -gt 100) 'Incomplete package manifest.'
    foreach($notice in @('ThirdParty/dotnet/LICENSE.TXT','ThirdParty/dotnet/THIRD-PARTY-NOTICES.TXT')){Check (Test-Path -LiteralPath (Join-Path $Directory $notice)) 'Missing published .NET runtime notice.'}
    foreach($file in $manifest.files){$path=Join-Path $Directory $file.path;Check ((Get-Item -LiteralPath $path).Length -eq $file.bytes -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $file.sha256) ('Package hash mismatch: '+$file.path)}
    return $manifest
}
function Inputs([string]$Directory) {
    return (@('project.json','build.json','Scenes/Main.scene.json','Scripts/Main.bindings.json','Scripts/Spin.cs') | ForEach-Object {(Get-FileHash -LiteralPath (Join-Path $Directory $_) -Algorithm SHA256).Hash}) -join ','
}
$before=Inputs $project
Check ((Export $project $name (Join-Path $root 'export.log')) -eq 0 -and (Test-Path -LiteralPath $package)) 'Successful game export failed.'
$manifest=VerifyManifest $package;Check ((Inputs $project) -eq $before) 'Export altered project inputs.'
$launch=Get-Content -LiteralPath (Join-Path $package 'launch.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Check (!$launch.menu -and $launch.scriptType -eq 'emptygame.spin' -and $launch.bindings -and $launch.assetCache -eq 'Project/.cache/assets') 'Template build settings were not used.'
$processed=Get-Content -LiteralPath (Join-Path $package 'processed-assets.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($processed.files.Count -ge 2) 'Processed model cache was not packaged.'
$runtime=Get-Content -LiteralPath (Join-Path $package 'runtime-dependencies.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Check (!$runtime.sdk_required -and !$runtime.clean_machine_tested -and $runtime.crt.Count -ge 3 -and (Test-Path -LiteralPath (Join-Path $package 'coreclr.dll')) -and (Test-Path -LiteralPath (Join-Path $package 'hostfxr.dll'))) 'Missing self-contained runtime files.'
$relocated=Join-Path $root '中文 路径/Game';New-Item -ItemType Directory -Path (Split-Path -Parent $relocated) -Force | Out-Null;Copy-Item -LiteralPath $package -Destination $relocated -Recurse
[void](VerifyManifest $relocated)
$outside=Join-Path $root 'OtherWorkingDirectory';New-Item -ItemType Directory -Path $outside | Out-Null
try {
    Set-Location -LiteralPath $outside
    Check ((Invoke-EngineProgram $powershell @('-NoProfile','-File',(Join-Path $relocated 'Start-Game.ps1'),'-Hidden','-Frames','12','-Capture',(Join-Path $root 'relocated.ppm'),'-CaptureFrame','12','-Locale','en-US') (Join-Path $root 'relocated.log')) -eq 0) 'Relocated game window failed.'
    Check ((Invoke-EngineProgram $powershell @('-NoProfile','-File',(Join-Path $relocated 'Start-Game.ps1'),'-Editor','-Play','-Hidden','-Frames','6','-Locale','en-US') (Join-Path $root 'relocated-editor.log')) -eq 0) 'Relocated editor Play failed.'
} finally {Set-Location -LiteralPath $repository}
foreach($file in $processed.files){Check ((Get-FileHash -LiteralPath (Join-Path $relocated $file.path) -Algorithm SHA256).Hash.ToLowerInvariant() -eq $file.sha256) 'Relocated runtime changed or rebuilt the supplied model cache.'}
$info=New-Object Diagnostics.ProcessStartInfo;$info.FileName=Join-Path $relocated 'SelfEngine.Bootstrap.exe';$info.WorkingDirectory=$outside;$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
$restrictedArguments=@('--project-root',(Join-Path $relocated 'Project'),'--scene',(Join-Path $relocated $launch.scene),'--asset-root',(Join-Path $relocated $launch.assetRoot),'--asset-cache',(Join-Path $relocated $launch.assetCache),'--project',(Join-Path $relocated $launch.scriptAssembly),'--bindings',(Join-Path $relocated $launch.bindings),'--ui',(Join-Path $relocated $launch.ui),'--window','--hidden','--frames','3','--locale','en-US')
$info.Arguments=(@(foreach($argument in $restrictedArguments){$value=[regex]::Replace($argument,'(\\*)"','$1$1\"');$value=[regex]::Replace($value,'(\\+)$','$1$1');'"'+$value+'"'}) -join ' ')
$info.EnvironmentVariables['PATH']=(Join-Path $env:SystemRoot 'System32')+';'+$env:SystemRoot
$child=New-Object Diagnostics.Process;$child.StartInfo=$info
try{[void]$child.Start();$stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync();$child.WaitForExit();[IO.File]::WriteAllText((Join-Path $root 'restricted-path.log'),$stdout.Result+$stderr.Result,(New-Object Text.UTF8Encoding($false)));Check ($child.ExitCode -eq 0) 'App-local runtime failed with a PATH containing no SDK.'}finally{$child.Dispose()}
$failureRecords=@()
function Failed([string]$Directory,[string]$CaseName,[string]$ExpectedPhase,[string]$ExpectedText) {
    $failedName=$name+'-'+$CaseName;$snapshot=Inputs $Directory;$log=Join-Path $root ($CaseName+'.log')
    Check ((Export $Directory $failedName $log) -ne 0 -and !(Test-Path -LiteralPath (Join-Path $repository ('out/packages/'+$failedName)))) ('Invalid project was published: '+$CaseName)
    $case=Get-ChildItem -LiteralPath (Join-Path $repository 'out/exports') -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $report=Get-Content -LiteralPath (Join-Path $case.FullName 'export-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Check (!$report.success -and $report.phase -eq 'failed' -and $report.failed_phase -eq $ExpectedPhase -and (Inputs $Directory) -eq $snapshot) ('Failure lost its phase or project data: '+$CaseName)
    $detail=if($ExpectedPhase -eq 'input_validation'){[string]$report.error}else{Get-Content -LiteralPath (Join-Path $case.FullName ($ExpectedPhase+'.log')) -Raw -Encoding UTF8}
    Check ($detail.Contains($ExpectedText)) ('Failure lacks its exact cause: '+$CaseName)
    return [ordered]@{case=$CaseName;phase=$ExpectedPhase;report=$case.FullName;success=$false;source_preserved=$true}
}
function Clone([string]$CaseName){$path=Join-Path $root $CaseName;Copy-Item -LiteralPath $project -Destination $path -Recurse;return $path}
$bad=Clone 'missing-resource';Remove-Item -LiteralPath (Join-Path $bad 'Content/Models/checker.png');$failureRecords+=Failed $bad 'missing-resource' 'content_validation' 'Project.MissingDependency'
$bad=Clone 'unknown-component';$json=Get-Content -LiteralPath (Join-Path $bad 'Scenes/Main.scene.json') -Raw -Encoding UTF8 | ConvertFrom-Json;if($json.version -eq 2){$json.entities[0].components=@($json.entities[0].components)+[pscustomobject]@{type='999999';version=1;enabled=$true;properties=@{}}}else{$json.entities[0].components | Add-Member -NotePropertyName unsupported_audio -NotePropertyValue @{}};$json | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $bad 'Scenes/Main.scene.json') -Encoding UTF8;$failureRecords+=Failed $bad 'unknown-component' 'content_validation' 'Scene.'
$bad=Clone 'bad-field';$json=Get-Content -LiteralPath (Join-Path $bad 'Scripts/Main.bindings.json') -Raw -Encoding UTF8 | ConvertFrom-Json;$json.Scripts[0].Fields.RadiansPerSecond.Kind='i32';$json | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $bad 'Scripts/Main.bindings.json') -Encoding UTF8;$failureRecords+=Failed $bad 'bad-field' 'field_preflight' 'RadiansPerSecond'
$bad=Clone 'bad-code';Add-Content -LiteralPath (Join-Path $bad 'Scripts/Spin.cs') -Value 'this is invalid C#;' -Encoding UTF8;$failureRecords+=Failed $bad 'bad-code' 'publish_scripts' 'Spin.cs('
$bad=Clone 'outside-ui';$json=Get-Content -LiteralPath (Join-Path $bad 'build.json') -Raw -Encoding UTF8 | ConvertFrom-Json;$json.entryUi='../outside.ui.json';$json | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $bad 'build.json') -Encoding UTF8;$failureRecords+=Failed $bad 'outside-ui' 'input_validation' 'Export.PathOutsideRoot'
Check ((Export $project $name (Join-Path $root 'existing-output.log')) -ne 0) 'Existing export was overwritten.';[void](VerifyManifest $package)
$leasePath=Join-Path $repository 'out/preview/preview.lock';$lease=[IO.File]::Open($leasePath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
try{$lease.Lock(1,1);Check ((Export $project ($name+'-busy') (Join-Path $root 'busy.log')) -ne 0 -and !(Test-Path -LiteralPath (Join-Path $repository ('out/packages/'+$name+'-busy')))) 'Active preview did not prevent export.'}finally{$lease.Dispose()}
Write-EngineJson (Join-Path $root 'verification.json') ([ordered]@{schema=1;success=$true;project=$project;package=$package;relocated=$relocated;authoring=$true;preview=$true;package_files=$manifest.file_count;self_contained=$true;processed_model_cache_unchanged=$true;outside_working_directory=$true;unicode_path=$true;restricted_path=$true;clean_machine_tested=$false;existing_output_preserved=$true;active_lease_rejected=$true;failures=$failureRecords})
Write-Output ('PackageExportTests passed: '+$root)
