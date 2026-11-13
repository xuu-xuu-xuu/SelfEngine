add_library(EngineGraphics STATIC
    Source/PlatformLayer/Graphics/VisibilityData.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Visibility.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Indirect.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12HiZ.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Exposure.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Taa.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12DepthPrepass.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Gui.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12SceneOverlay.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Forward.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Shadow.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12PostProcess.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12PipelineCache.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Memory.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Descriptors.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12UploadArena.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12StagedUpload.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12StateTracker.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12GpuTimer.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Dred.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12PipelineLibraryStorage.cpp
    Source/PlatformLayer/Graphics/D3D12/D3D12Context.cpp Source/PlatformLayer/Graphics/D3D12/D3D12Presentation.cpp Source/PlatformLayer/Graphics/D3D12/D3D12Triangle.cpp Source/PlatformLayer/Graphics/D3D12/D3D12Mesh.cpp Source/PlatformLayer/Graphics/D3D12/D3D12Texture.cpp Source/PlatformLayer/Graphics/D3D12/D3D12Buffer.cpp Source/PlatformLayer/Graphics/D3D12/D3D12Rhi.cpp Source/PlatformLayer/Graphics/D3D12/D3D12Shader.cpp Source/PlatformLayer/Graphics/D3D12/D3D12Result.cpp)
se_target(EngineGraphics)
if(MSVC)
    set_source_files_properties(Source/PlatformLayer/Graphics/VisibilityData.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
target_link_libraries(EngineGraphics PUBLIC EngineCore PRIVATE EnginePlatform d3dcompiler d3d12 dxgi)

add_library(EngineSandboxRenderer STATIC
    Source/FunctionLayer/Rendering/SandboxRenderer.cpp
    Source/FunctionLayer/Rendering/StaticModelRender.cpp
    Source/FunctionLayer/Rendering/Visibility.cpp
    Source/FunctionLayer/Rendering/RenderQueue.cpp
    Source/FunctionLayer/Rendering/Instancing.cpp
    Source/FunctionLayer/Rendering/ForwardRenderer.cpp
    Source/FunctionLayer/Rendering/ShadowSystem.cpp
    Source/FunctionLayer/Rendering/CascadeShadow.cpp
    Source/FunctionLayer/Rendering/LightClustering.cpp
    Source/FunctionLayer/Rendering/AmbientOcclusion.cpp
    Source/FunctionLayer/Rendering/Atmosphere.cpp
    Source/FunctionLayer/Rendering/Reflection.cpp
    Source/FunctionLayer/Rendering/FrameGraph.cpp
    Source/FunctionLayer/Rendering/TemporalAA.cpp
    Source/FunctionLayer/Rendering/AutoExposure.cpp
    Source/FunctionLayer/Rendering/EnvironmentRender.cpp
    Source/FunctionLayer/Rendering/PostProcessRenderer.cpp
    Source/FunctionLayer/Rendering/AssetUploadScheduler.cpp
    Source/FunctionLayer/Rendering/GuiRenderer.cpp
    Source/FunctionLayer/Rendering/EditorViewportTarget.cpp
    Source/FunctionLayer/Rendering/GuiCompositor.cpp
    Source/FunctionLayer/Rendering/GameUIRenderer.cpp
    Source/FunctionLayer/Rendering/OcclusionHud.cpp
    Source/FunctionLayer/Rendering/PhysicsDebugDraw.cpp
    Source/FunctionLayer/Rendering/RenderTelemetry.cpp
    Source/FunctionLayer/Rendering/PipelineWarmup.cpp
    Source/FunctionLayer/Rendering/TextureResidency.cpp
    Source/FunctionLayer/Rendering/LodSelect.cpp
    Source/FunctionLayer/Rendering/HlodSelect.cpp
    Source/FunctionLayer/Rendering/HlodStream.cpp
    Source/FunctionLayer/Rendering/HlodStreamer.cpp
    Source/FunctionLayer/Rendering/HlodProxyCache.cpp
    Source/FunctionLayer/Rendering/HiZ.cpp)
se_target(EngineSandboxRenderer)
target_link_libraries(EngineSandboxRenderer PUBLIC EnginePlatform EngineRuntime EngineGraphics EngineColorGrading PRIVATE EngineAssets)
