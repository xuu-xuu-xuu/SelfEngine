param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [Parameter(Mandatory=$true)][string]$ScriptsProject,
    [Parameter(Mandatory=$true)][string]$Scene,
    [Parameter(Mandatory=$true)][string]$Bindings,
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [ValidateSet('Editor','Window','Headless')][string]$Mode='Editor',
    [ValidateSet('zh-CN','en-US')][string]$Locale='zh-CN',
    [ValidateRange(0,100000)][int]$Frames=0,
    [switch]$Hidden,[switch]$Play
)
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
$ErrorActionPreference='Stop'
$repository=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $repository
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'Copy-RendererResources.ps1')
. (Join-Path $PSScriptRoot 'ProjectBuildLease.ps1')
$texts=@{}
Get-Content -LiteralPath (Join-Path $repository ('Resources/Localization/'+$Locale+'.txt')) -Encoding UTF8 | ForEach-Object {
    if($_ -match '^([^#=]+)=(.*)$'){$texts[$matches[1]]=$matches[2]}
}
function Text([string]$Key){if($texts.ContainsKey($Key)){return $texts[$Key]};return $Key}
function ProjectFile([string]$Path){if([IO.Path]::IsPathRooted($Path)){return [IO.Path]::GetFullPath($Path)};return [IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))}
function WriteReport {
    [IO.File]::WriteAllText((Join-Path $case 'preview.json'),($report | ConvertTo-Json -Depth 10),(New-Object Text.UTF8Encoding($false)))
}
$lease=$null;$child=$null;$exitCode=1;$case=$null;$report=$null
try {
    $ProjectRoot=(Resolve-Path -LiteralPath $ProjectRoot).Path
    if(!(Test-Path -LiteralPath $ProjectRoot -PathType Container)){throw (Text 'Preview.InvalidProject')}
    $project=ProjectFile $ScriptsProject;$sceneFile=ProjectFile $Scene;$bindingsFile=ProjectFile $Bindings
    foreach($file in @($project,$sceneFile,$bindingsFile)){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw ((Text 'Preview.InvalidProject')+': '+$file)}}
    $native=Join-Path $repository ('out/build/windows-x64-'+$Configuration.ToLowerInvariant())
    if(!(Test-Path -LiteralPath (Join-Path $native 'EngineNative.dll'))){throw (Text 'Preview.NativeMissing')}
    try {
        $leaseInfo=Open-EngineBuildLease $repository;$lease=$leaseInfo.Stream;$leasePath=$leaseInfo.Path
    } catch {if($lease){$lease.Dispose();$lease=$null};throw (Text 'Preview.Busy')}
    $case=Join-Path $ProjectRoot ('out/Preview/'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $case -Force | Out-Null
    $report=[ordered]@{schema=1;success=$false;phase='restore_bootstrap';project_root=$ProjectRoot;configuration=$Configuration;mode=$Mode;script_project=$project;scene=$sceneFile;bindings=$bindingsFile;process_id=$null;exit_code=$null}
    WriteReport
    $dotnet=(Get-Command dotnet.exe -ErrorAction Stop).Source
    $bootstrapProject=Join-Path $repository 'Source/FunctionLayer/Scripting/Bootstrap/Bootstrap.csproj'
    $restoreArguments=@('restore',$bootstrapProject,'--configfile',(Join-Path $repository 'NuGet.Config'),'--packages',(Join-Path $repository 'out/nuget/packages'),('-p:Configuration='+$Configuration),'--nologo')
    if((Invoke-EngineProgram $dotnet $restoreArguments (Join-Path $case 'bootstrap-restore.log')) -ne 0){throw (Text 'Preview.RestoreFailed')}
    $report.phase='compile_bootstrap';WriteReport
    $compileArguments=@('build',$bootstrapProject,'--configuration',$Configuration,'--nologo','--no-restore')
    if((Invoke-EngineProgram $dotnet $compileArguments (Join-Path $case 'bootstrap-build.log')) -ne 0){throw (Text 'Preview.BuildFailed')}
    $report.phase='restore_project';WriteReport
    $scriptOutput=Join-Path $case 'Scripts'
    $restoreArguments=@('restore',$project,'--configfile',(Join-Path $repository 'NuGet.Config'),'--packages',(Join-Path $repository 'out/nuget/packages'),('-p:Configuration='+$Configuration),('-p:SelfEngineRoot='+$repository),'--nologo')
    if((Invoke-EngineProgram $dotnet $restoreArguments (Join-Path $case 'project-restore.log')) -ne 0){throw (Text 'Preview.RestoreFailed')}
    $report.phase='compile_project';WriteReport
    $compileArguments=@('build',$project,'--configuration',$Configuration,'--output',$scriptOutput,('-p:SelfEngineRoot='+$repository),'--nologo','--no-restore')
    if((Invoke-EngineProgram $dotnet $compileArguments (Join-Path $case 'project-build.log')) -ne 0){throw (Text 'Preview.BuildFailed')}
    $nameLog=Join-Path $case 'assembly-name.log'
    if((Invoke-EngineProgram $dotnet @('msbuild',$project,'-nologo',('-p:Configuration='+$Configuration),('-p:SelfEngineRoot='+$repository),'-getProperty:TargetFileName') $nameLog) -ne 0){throw (Text 'Preview.BuildFailed')}
    $assemblyName=(Get-Content -LiteralPath $nameLog -Raw -Encoding UTF8).Trim()
    if([IO.Path]::GetFileName($assemblyName) -ne $assemblyName -or !($assemblyName.EndsWith('.dll'))){throw (Text 'Preview.BuildFailed')}
    $assembly=Join-Path $scriptOutput $assemblyName
    if(!(Test-Path -LiteralPath $assembly)){throw ((Text 'Preview.BuildFailed')+': '+$assembly)}
    $binaryDirectory=Join-Path $repository ('out/managed/Bootstrap/'+$Configuration+'/net10.0')
    Copy-Item -LiteralPath (Join-Path $native 'EngineNative.dll') -Destination (Join-Path $binaryDirectory 'EngineNative.dll') -Force
    $resources=Join-Path $binaryDirectory 'Resources';New-Item -ItemType Directory -Path $resources -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repository 'Resources/Localization') -Destination $resources -Recurse -Force
    Copy-EngineRendererResources -NativeDirectory $native -Destination $resources
    $arguments=@((Join-Path $binaryDirectory 'SelfEngine.Bootstrap.dll'),'--project-root',$ProjectRoot,'--project',$assembly,'--scene',$sceneFile,'--bindings',$bindingsFile,'--locale',$Locale,'--preview-lease',$leasePath)
    if($Mode -eq 'Editor'){$arguments+='--editor';if($Play){$arguments+='--play'}}
    elseif($Mode -eq 'Window'){$arguments+='--window'}
    elseif($Frames -eq 0){$Frames=120}
    if($Hidden){$arguments+='--hidden'}
    if($Frames -gt 0){$arguments+=@('--frames',[string]$Frames)}
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$dotnet
    $info.Arguments=(@(foreach($argument in $arguments){$value=[regex]::Replace($argument,'(\\*)"','$1$1\"');$value=[regex]::Replace($value,'(\\+)$','$1$1');'"'+$value+'"'}) -join ' ')
    $info.WorkingDirectory=$repository;$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.StandardOutputEncoding=[Text.Encoding]::UTF8;$info.StandardErrorEncoding=[Text.Encoding]::UTF8
    $child=New-Object Diagnostics.Process;$child.StartInfo=$info
    $report.phase='starting';$report.assembly=$assembly;$report.assembly_sha256=(Get-FileHash -LiteralPath $assembly -Algorithm SHA256).Hash.ToLowerInvariant();WriteReport
    $lease.Unlock(1,1) # 子进程在创建 EngineSession 之前取得第 1 字节。
    [void]$child.Start();$report.process_id=$child.Id;$report.process_start_utc=$child.StartTime.ToUniversalTime().ToString('o');$report.phase='running';WriteReport
    $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
    $child.WaitForExit();$exitCode=$child.ExitCode
    [IO.File]::WriteAllText((Join-Path $case 'preview.log'),$stdout.Result+$stderr.Result,(New-Object Text.UTF8Encoding($false)))
    $report.exit_code=$exitCode;$report.success=$exitCode -eq 0;$report.phase=if($report.success){'complete'}else{'run_failed'};WriteReport
    if($exitCode -ne 0){throw (Text 'Preview.RunFailed')}
    Write-Output ((Text 'Preview.Completed')+': '+$case)
} catch {
    if($report){$report.error=$_.Exception.Message;$report.failed_phase=$report.phase;if($report.phase -ne 'run_failed'){$report.phase='failed'};WriteReport}
    Write-Error -Message ($_.Exception.Message+$(if($case){': '+$case}else{''})) -ErrorAction Continue
    $exitCode=1
} finally {
    if($child){$child.Dispose()}
    if($lease){$lease.Dispose()}
}
exit $exitCode
