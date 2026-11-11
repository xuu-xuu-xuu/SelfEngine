# 统一预览/导出协议：构建持有字节 0、1，预览子进程持有字节 1。
function Open-EngineBuildLease([string]$Repository) {
    $directory=Join-Path $Repository 'out/preview'
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $path=Join-Path $directory 'preview.lock';$stream=$null
    try {
        $stream=[IO.File]::Open($path,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
        $stream.Lock(0,1);$stream.Lock(1,1)
        return [pscustomobject]@{Stream=$stream;Path=$path}
    } catch {
        if($stream){$stream.Dispose()}
        throw [IO.IOException]::new('Preview.Busy',$_.Exception)
    }
}
