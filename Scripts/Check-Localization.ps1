$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$strictUtf8 = [Text.UTF8Encoding]::new($false,$true)
$tables = @{}
foreach ($locale in @('zh-CN','en-US')) {
    $table = @{}
    $path = Join-Path $root ('Resources/Localization/' + $locale + '.txt')
    foreach ($line in $strictUtf8.GetString([IO.File]::ReadAllBytes($path)).Split("`n")) {
        $text = $line.Trim()
        if (!$text -or $text.StartsWith('#')) { continue }
        $parts = $text.Split('=',2)
        if ($parts.Count -ne 2 -or !$parts[1] -or $table.ContainsKey($parts[0])) { throw "Invalid or duplicate localization row: $locale / $text" }
        $table[$parts[0]] = $parts[1]
    }
    $tables[$locale] = $table
}
foreach ($key in $tables['zh-CN'].Keys) { if (!$tables['en-US'].ContainsKey($key)) { throw "Missing English key: $key" } }
foreach ($key in $tables['en-US'].Keys) { if (!$tables['zh-CN'].ContainsKey($key)) { throw "Missing Chinese key: $key" } }
$missing = [Collections.Generic.HashSet[string]]::new()
$sourceRoots = @('Source', 'Samples', 'Tests/Managed') | ForEach-Object { Join-Path $root $_ }
foreach ($file in Get-ChildItem -LiteralPath $sourceRoots -Recurse -File | Where-Object { $_.Extension -in '.h','.cpp','.cs' -and $_.FullName -notmatch '[\\/](bin|obj)[\\/]' }) {
    $source = $strictUtf8.GetString([IO.File]::ReadAllBytes($file.FullName))
    foreach ($match in [regex]::Matches($source,'"((?:Assets|Bootstrap|Bridge|Clock|Component|Editor|EditorUi|Event|Tick|Font|Metadata|Material|MenuFlow|Physics|Profiler|Project|Property|Sample|Scene|Script|Tasks|UI|World|App|D3D12|Timer|Window)\.[A-Za-z][A-Za-z0-9_.]+)"')) {
        if (!$tables['zh-CN'].ContainsKey($match.Groups[1].Value)) { [void]$missing.Add($match.Groups[1].Value) }
    }
}
if ($missing.Count) { throw ('Missing localization keys: ' + (($missing | Sort-Object) -join ', ')) }
Write-Output ('Localization checked: ' + $tables['zh-CN'].Count + ' bilingual keys; source UTF-8 valid.')
