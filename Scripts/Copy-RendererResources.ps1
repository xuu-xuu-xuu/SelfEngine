function Copy-EngineRendererResources {
    param([Parameter(Mandatory=$true)][string]$NativeDirectory,[Parameter(Mandatory=$true)][string]$Destination)
    $source=Join-Path $NativeDirectory 'Resources'
    foreach($required in @('Shaders/Gui.hlsl','Shaders/Gui.hlsl.sebin','Shaders/Pbr.hlsl','Shaders/Pbr.hlsl.sebin','Shaders/TexturedMesh.hlsl','Shaders/TexturedMesh.hlsl.sebin','Textures/checker.png')) {
        if(-not (Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)){throw ('Renderer resource missing; build CopyRendererResources first: '+$required)}
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach($folder in @('Shaders','Textures')) {
        Copy-Item -LiteralPath (Join-Path $source $folder) -Destination $Destination -Recurse -Force
    }
}
