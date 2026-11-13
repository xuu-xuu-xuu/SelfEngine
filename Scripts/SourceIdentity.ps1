function Write-EngineJson([string]$Path,$Value){[IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 24)+"`n",[Text.UTF8Encoding]::new($false))}
if($PSVersionTable.PSVersion.Major -le 5){
    # 从 PowerShell 7 的 .NET 子进程启动时可能继承其模块路径，显式使用本宿主的 Utility。
    Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
}
function Get-EngineSourceIdentity([string]$ProjectRoot){
    $ProjectRoot=(Resolve-Path -LiteralPath $ProjectRoot).Path.TrimEnd('\')
    $inputPaths=[Collections.Generic.List[string]]::new()
    # 仅计算可发布输入，排除本地保留的回归、实验和旧路径原件。
    $candidates=& git -C $ProjectRoot -c core.quotepath=false ls-files --cached --others --exclude-standard
    if($LASTEXITCODE -ne 0){throw 'Cannot enumerate engine source inputs.'}
    foreach($relative in @($candidates | Sort-Object -Unique)){
        if($relative -match '^(Source|Resources|Scripts|ThirdParty)/' -and $relative -notmatch '/(bin|obj|__pycache__|\.cache)/' -and [IO.Path]::GetExtension($relative) -notin @('.pyc','.pyo','.md')){
            $inputPaths.Add((Join-Path $ProjectRoot $relative))
        }
    }
    foreach($name in @('CMakeLists.txt','CMakePresets.json')){$inputPaths.Add((Join-Path $ProjectRoot $name))}
    # 也允许对迁移前的冻结源树计算原有摘要。
    $configRoot=Join-Path $ProjectRoot 'BuildConfig'
    if(!(Test-Path -LiteralPath $configRoot -PathType Container)){$configRoot=$ProjectRoot}
    foreach($name in @('Directory.Build.props','NuGet.Config')){$inputPaths.Add((Join-Path $configRoot $name))}
    $buildTargets=Join-Path $configRoot 'Directory.Build.targets'
    if(Test-Path -LiteralPath $buildTargets){$inputPaths.Add($buildTargets)} # 兼容旧冻结版本中尚无此文件的源树。
    $sortedPaths=$inputPaths.ToArray();[Array]::Sort($sortedPaths,[StringComparer]::Ordinal)
    $sourceFiles=@($sortedPaths|ForEach-Object{
        $sourcePath=$_;$digest=(Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
        if(!$digest){throw ('Cannot hash source input: '+$sourcePath)}
        [ordered]@{path=$sourcePath.Substring($ProjectRoot.Length+1).Replace('\','/');sha256=$digest.ToLowerInvariant()}
    })
    $text=($sourceFiles|ForEach-Object{$_.path+"`0"+$_.sha256+"`n"}) -join ''
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$identity=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-','').ToLowerInvariant()}
    finally{$sha.Dispose()}
    $head=(& git -C $ProjectRoot rev-parse HEAD).Trim();if($LASTEXITCODE -ne 0){throw 'Cannot identify Git base.'}
    $dirty=[bool](& git -C $ProjectRoot status --porcelain)
    return [ordered]@{schema=1;identity='SHA256 UTF8 path NUL digest LF, ordinal path order';excludes=@('**/bin/**','**/obj/**','**/__pycache__/**','**/.cache/**','*.pyc','*.pyo','*.md');git_head=$head;dirty=$dirty;source_sha256=$identity;files=$sourceFiles}
}
