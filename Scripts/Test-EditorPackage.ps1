param(
    [Parameter(Mandatory=$true)][string]$Package,
    [string]$Evidence='',
    [string]$RepositoryRoot='',
    [switch]$SkipScriptCompilation
)
$ErrorActionPreference='Stop'
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'EditorPackageRuntime.ps1')
$package=(Resolve-Path -LiteralPath $Package).Path
if(!(Test-Path -LiteralPath (Join-Path $package 'editor-package.json'))){throw 'editor-package.json is missing.'}
if(!$Evidence){$stamp=[guid]::NewGuid().ToString('N').Substring(0,8);$Evidence=Join-Path $package ('..\..\validation\editor-product\U09-verify-'+$stamp)}
$Evidence=[IO.Path]::GetFullPath($Evidence)
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$repository=if($RepositoryRoot){[IO.Path]::GetFullPath($RepositoryRoot)}else{(Resolve-Path (Join-Path $PSScriptRoot '..')).Path}
$manifest=Get-Content -LiteralPath (Join-Path $package 'editor-package.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$result=[ordered]@{schema=1;success=$false;package=$package;evidence=$Evidence;checks=@();error=''}
$resultPath=Join-Path $Evidence 'package-verify.json'
function Check([string]$name,[bool]$value,[string]$detail='') {
    $result.checks+=@([ordered]@{name=$name;passed=$value;detail=$detail})
    if(!$value){throw ('Package check failed: '+$name+' '+$detail)}
}
function SaveResult {[IO.File]::WriteAllText($resultPath,($result|ConvertTo-Json -Depth 8)+"`n",[Text.UTF8Encoding]::new($false))}
function RunOutside([string]$name,[string]$program,[string[]]$arguments){
    Push-Location -LiteralPath $env:TEMP
    try{return Invoke-EngineProgram $program $arguments (Join-Path $Evidence ($name+'.log'))}
    finally{Pop-Location}
}
try {
    SaveResult
    [void](Read-EngineEditorPackage $package)
    Check 'manifest_schema' ($manifest.schema -eq 1 -and $manifest.kind -eq 'SelfEngine.EditorPackage') ('schema='+$manifest.schema+' kind='+$manifest.kind)
    Check 'manifest_name' ($manifest.name -eq (Split-Path -Leaf $package)) $manifest.name
    $files=@(Get-ChildItem -LiteralPath $package -File -Recurse | Where-Object Name -ne 'editor-package.json')
    $listed=@{};foreach($item in $manifest.files){$listed[$item.path]=$item.sha256}
    $missing=@($files | Where-Object {!$listed.ContainsKey($_.FullName.Substring($package.Length+1).Replace('\','/'))})
    Check 'manifest_covers_files' ($missing.Count -eq 0) ('missing='+$missing.Count)
    $mismatch=@();foreach($path in $listed.Keys){$target=Join-Path $package ($path.Replace('/','\'));if(!(Test-Path -LiteralPath $target)){$mismatch+=@($path);continue};if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $listed[$path]){$mismatch+=@($path)}}
    Check 'manifest_hashes' ($mismatch.Count -eq 0) ('mismatch='+($mismatch -join ','))
    $forbidden=@($files | Where-Object {$_.FullName -match '[\\/]out[\\/]Editor[\\/]User[\\/]|[\\/]\.git[\\/]|[\\/]\.cache[\\/]|\\bin\\|\\obj\\'})
    Check 'no_forbidden_paths' ($forbidden.Count -eq 0) ('count='+$forbidden.Count)
    Check 'source_identity' (![string]::IsNullOrWhiteSpace($manifest.source_sha256) -and ![string]::IsNullOrWhiteSpace($manifest.git_head)) ($manifest.source_sha256+' '+$manifest.git_head)
    $exe=Join-Path $package 'SelfEngineEditor.exe'
    $launcherLog=Join-Path $Evidence 'launcher.log'
    $capture=Join-Path $Evidence 'launcher.bmp'
    $launcherCode=RunOutside 'launcher' $exe @('--smoke-launcher','600','--capture-launcher',$capture,'--no-ui-errors')
    Check 'launcher_outside_cwd' ($launcherCode -eq 0 -and (Test-Path -LiteralPath $capture)) ('exit='+$launcherCode+' capture='+(Test-Path -LiteralPath $capture))
    $readyTimes=@()
    foreach($round in 1..5){
        $timingCode=RunOutside ('launcher-timing-'+$round) $exe @('--smoke-launcher','100','--no-ui-errors')
        $timingText=Get-Content -LiteralPath (Join-Path $Evidence ('launcher-timing-'+$round+'.log')) -Raw -Encoding UTF8
        Check ('launcher_timing_'+$round) ($timingCode -eq 0 -and $timingText -match 'launcher-interactive-ms=(\d+)')
        $readyTimes+=@([long]$Matches[1])
    }
    $median=(@($readyTimes|Sort-Object))[2]
    $result.launcherTiming=[ordered]@{samplesMs=$readyTimes;medianMs=$median;budgetMs=3000;measurement='OS process creation to visible enabled controls and synchronous initial paint; excludes smoke wait and capture delay';manualInputVerified=$false}
    Check 'warm_launcher_ready_budget' ($median -ge 0 -and $median -le 3000) ('median_ms='+$median)
    $projectParent=Join-Path $Evidence 'projects'
    New-Item -ItemType Directory -Path $projectParent -Force | Out-Null
    $createLog=Join-Path $Evidence 'create.log'
    $createCode=RunOutside 'create' $exe @('--smoke-create',$projectParent,'--no-ui-errors')
    $created=@(Get-ChildItem -LiteralPath $projectParent -Directory -Filter 'SmokeProject*' -ErrorAction SilentlyContinue)
    if($created.Count -eq 0){$created=@(Get-ChildItem -LiteralPath $projectParent -Directory -ErrorAction SilentlyContinue)}
    $hasProject=$created.Count -gt 0 -and (Test-Path -LiteralPath (Join-Path $created[0].FullName 'project.json'))
    Check 'template_create_outside_package' ($createCode -eq 0 -and $hasProject) ('exit='+$createCode+' project='+$hasProject)
    $previousPath=$env:PATH
    try {
        $env:PATH=$env:SystemRoot+'\System32;'+$env:SystemRoot
        $restrictedCode=RunOutside 'restricted-path' $exe @('--smoke-launcher','400','--no-ui-errors')
    }
    finally {$env:PATH=$previousPath}
    Check 'restricted_path_startup' ($restrictedCode -eq 0) ('exit='+$restrictedCode)
    $userRoot=Join-Path $env:LOCALAPPDATA 'SelfEngine/Editor'
    $localUserData=Test-Path -LiteralPath $userRoot
    $packageUserData=Test-Path -LiteralPath (Join-Path $package 'out/Editor/User')
    Check 'user_data_outside_package' ($localUserData -and !$packageUserData) ('local='+$userRoot+' package_local='+$packageUserData)
    $manifestHash=(Get-FileHash -LiteralPath (Join-Path $package 'editor-package.json') -Algorithm SHA256).Hash
    $powershell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $sameNameCode=RunOutside 'same-name' $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $repository 'Scripts/Build-EditorPackage.ps1'),'-Configuration',$manifest.configuration,'-Name',$manifest.name,'-OutputDirectory',(Split-Path -Parent $package))
    $unchanged=$manifestHash -eq (Get-FileHash -LiteralPath (Join-Path $package 'editor-package.json') -Algorithm SHA256).Hash
    Check 'same_name_refused' ($sameNameCode -ne 0 -and $unchanged) ('exit='+$sameNameCode+' manifest_unchanged='+$unchanged)
    $exportArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'Test-EditorPackageExport.ps1'),'-Package',$package,'-Evidence',(Join-Path $Evidence 'export'))
    if($SkipScriptCompilation){$exportArguments+='-SkipScriptCompilation'}
    $exportCode=RunOutside 'package-export' $powershell $exportArguments
    Check 'package_export' ($exportCode -eq 0) ('exit='+$exportCode+' report=export/package-export-verify.json')
    $result.success=$true
    SaveResult
    Write-Output ('U09 package verified: '+$package)
    Write-Output ('files='+$manifest.file_count+' bytes='+$manifest.total_bytes)
    exit 0
}
catch {
    $result.error=$_.Exception.Message
    SaveResult
    Write-Error ('Editor package verification failed: '+$_.Exception.Message) -ErrorAction Continue
    exit 1
}
