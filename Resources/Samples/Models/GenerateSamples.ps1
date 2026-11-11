# 重建本目录自制的 glTF、GLB 与 2×2 PNG；不读取或下载外部素材。
$ErrorActionPreference = 'Stop'
$sampleRoot = $PSScriptRoot
$utf8 = New-Object System.Text.UTF8Encoding($false)
$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
foreach ($value in @(0,0,0, 1,0,0, 0,2,0, 0,0,1, 0,0,1, 0,0,1, 0,0, 1,0, 0,1)) {
    $writer.Write([single]$value)
}
foreach ($value in @(0,1,2)) { $writer.Write([uint16]$value) }
$writer.Flush()
$binary = $stream.ToArray()
$writer.Dispose()
$stream.Dispose()
$document = [ordered]@{
    asset = @{ version='2.0'; generator='SelfEngine self-authored fixture' }
    scene = 0
    scenes = @(@{ nodes=@(0) })
    nodes = @(@{ name='Parent'; translation=@(1,0,0); children=@(1) }, @{ name='Triangle'; mesh=0 })
    meshes = @(@{ name='Triangle'; primitives=@(@{ attributes=@{ POSITION=0; NORMAL=1; TEXCOORD_0=2 }; indices=3; material=0 }) })
    materials = @(@{ name='Checker'; pbrMetallicRoughness=@{ baseColorFactor=@(1,1,1,1); metallicFactor=0; roughnessFactor=0.8; baseColorTexture=@{ index=0 } } })
    textures = @(@{ source=0 })
    images = @(@{ uri='checker.png' })
    buffers = @(@{ byteLength=$binary.Length; uri='Hierarchy.bin' })
    bufferViews = @(@{ buffer=0; byteOffset=0; byteLength=36; target=34962 }, @{ buffer=0; byteOffset=36; byteLength=36; target=34962 }, @{ buffer=0; byteOffset=72; byteLength=24; target=34962 }, @{ buffer=0; byteOffset=96; byteLength=6; target=34963 })
    accessors = @(@{ bufferView=0; componentType=5126; count=3; type='VEC3'; min=@(0,0,0); max=@(1,2,0) }, @{ bufferView=1; componentType=5126; count=3; type='VEC3' }, @{ bufferView=2; componentType=5126; count=3; type='VEC2' }, @{ bufferView=3; componentType=5123; count=3; type='SCALAR' })
}
[System.IO.File]::WriteAllBytes((Join-Path $sampleRoot 'Hierarchy.bin'), $binary)
[System.IO.File]::WriteAllText((Join-Path $sampleRoot 'Hierarchy.gltf'), ($document | ConvertTo-Json -Depth 20), $utf8)
$document.buffers[0].Remove('uri')
$jsonText = $document | ConvertTo-Json -Depth 20 -Compress
while (($utf8.GetByteCount($jsonText) % 4) -ne 0) { $jsonText += ' ' }
$jsonBytes = $utf8.GetBytes($jsonText)
$binaryLength = [uint32](($binary.Length + 3) -band -4)
$glb = New-Object System.IO.MemoryStream
$glbWriter = New-Object System.IO.BinaryWriter($glb)
$glbWriter.Write([uint32]0x46546C67)
$glbWriter.Write([uint32]2)
$glbWriter.Write([uint32](12 + 8 + $jsonBytes.Length + 8 + $binaryLength))
$glbWriter.Write([uint32]$jsonBytes.Length)
$glbWriter.Write([uint32]0x4E4F534A)
$glbWriter.Write($jsonBytes)
$glbWriter.Write($binaryLength)
$glbWriter.Write([uint32]0x004E4942)
$glbWriter.Write($binary)
for ($index = $binary.Length; $index -lt $binaryLength; ++$index) { $glbWriter.Write([byte]0) }
$glbWriter.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $sampleRoot 'Hierarchy.glb'), $glb.ToArray())
$glbWriter.Dispose()
$glb.Dispose()
Add-Type -AssemblyName System.Drawing
$bitmap = New-Object System.Drawing.Bitmap(2, 2)
try {
    $bitmap.SetPixel(0, 0, [System.Drawing.Color]::White)
    $bitmap.SetPixel(1, 0, [System.Drawing.Color]::Red)
    $bitmap.SetPixel(0, 1, [System.Drawing.Color]::Blue)
    $bitmap.SetPixel(1, 1, [System.Drawing.Color]::White)
    $bitmap.Save((Join-Path $sampleRoot 'checker.png'), [System.Drawing.Imaging.ImageFormat]::Png)
} finally { $bitmap.Dispose() }
Write-Output 'Generated Hierarchy.gltf, Hierarchy.glb, Hierarchy.bin and checker.png.'
