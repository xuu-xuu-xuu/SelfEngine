param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$directory = Join-Path (Split-Path -Parent $PSScriptRoot) 'Resources/Branding'
$source = [Drawing.Bitmap]::new((Join-Path $directory 'FlareMark.png'))
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)

try {
    if ($source.Width -ne $source.Height) { throw 'FlareMark.png must be square.' }
    $frames = foreach ($size in $sizes) {
        # 保留 B 款原图的完整画布，四角也使用原稿的不透明黑底。
        $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $buffer = [IO.MemoryStream]::new()
        try {
            $graphics = [Drawing.Graphics]::FromImage($bitmap)
            $attributes = [Drawing.Imaging.ImageAttributes]::new()
            try {
                $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $attributes.SetWrapMode([Drawing.Drawing2D.WrapMode]::TileFlipXY)
                $rect = [Drawing.Rectangle]::new(0, 0, $size, $size)
                $graphics.DrawImage($source, $rect, 0, 0, $source.Width, $source.Height, [Drawing.GraphicsUnit]::Pixel, $attributes)
            } finally {
                $attributes.Dispose()
                $graphics.Dispose()
            }
            $bitmap.Save($buffer, [Drawing.Imaging.ImageFormat]::Png)
            ,$buffer.ToArray()
        } finally {
            $buffer.Dispose()
            $bitmap.Dispose()
        }
    }
} finally { $source.Dispose() }

$stream = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16*$sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $edge = [byte]($sizes[$i] % 256)
        $writer.Write($edge)
        $writer.Write($edge)
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length)
        $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame) }
    $writer.Flush()
    [IO.File]::WriteAllBytes((Join-Path $directory 'Flare.ico'), $stream.ToArray())
} finally { $writer.Dispose() }
