cmake_minimum_required(VERSION 3.25)
project(SelfEngine VERSION 0.1.0 LANGUAGES C CXX)
enable_language(RC)
configure_file(Resources/Branding/FlareResources.rc.in "${CMAKE_CURRENT_BINARY_DIR}/FlareResources.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/FlareResources.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Branding/Flare.ico")
include("${CMAKE_CURRENT_SOURCE_DIR}/Scripts/DetectMsvcIncludes.cmake")
function(se_target target)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_include_directories(${target} SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty")
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_definitions(${target} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/W4;/permissive-;/utf-8>")
    endif()
endfunction()
add_library(EngineCore STATIC Source/CoreLayer/Log.cpp Source/CoreLayer/Localization.cpp Source/CoreLayer/Utf.cpp Source/CoreLayer/TaskQueue.cpp Source/CoreLayer/Profiler.cpp Source/CoreLayer/Hash.cpp)
target_sources(EngineCore PRIVATE Source/CoreLayer/Spatial/BoundsIndex.cpp)
se_target(EngineCore)
target_link_libraries(EngineCore PRIVATE bcrypt)
# UI Toolkit 阶段 B：平台无关核心；只链接 Core，不链接 Platform/Render/Editor。
add_library(EngineUiCore STATIC
    Source/FunctionLayer/UI/Core/UiElement.cpp
    Source/FunctionLayer/UI/Core/UiDocument.cpp
    Source/FunctionLayer/UI/Core/UiDocumentLoader.cpp
    Source/FunctionLayer/UI/Core/UiBinding.cpp
    Source/FunctionLayer/UI/Core/UiLayout.cpp
    Source/FunctionLayer/UI/Core/UiStyle.cpp
    Source/FunctionLayer/UI/Core/UiInput.cpp)
se_target(EngineUiCore)
target_link_libraries(EngineUiCore PUBLIC EngineCore)
add_library(EngineUiRender STATIC Source/FunctionLayer/Rendering/UiPaint.cpp)
se_target(EngineUiRender)
target_link_libraries(EngineUiRender PUBLIC EngineUiCore EngineCore)
add_library(EngineUiWidgets STATIC
    Source/FunctionLayer/UI/Widgets/UiTextEdit.cpp
    Source/FunctionLayer/UI/Widgets/UiWidgets.cpp)
se_target(EngineUiWidgets)
target_link_libraries(EngineUiWidgets PUBLIC EngineUiCore EngineCore)
add_library(EngineEditorUi STATIC
    Source/ToolLayer/Editor/EditorUi/EditorCommand.cpp
    Source/ToolLayer/Editor/EditorUi/EditorCommandRouter.cpp
    Source/ToolLayer/Editor/EditorUi/EditorDock.cpp
    Source/ToolLayer/Editor/EditorUi/EditorDockState.cpp
    Source/ToolLayer/Editor/EditorUi/EditorChrome.cpp
    Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.cpp
    Source/ToolLayer/Editor/EditorUi/EditorShell.cpp
    Source/ToolLayer/Editor/EditorUi/EditorPanels.cpp)
se_target(EngineEditorUi)
target_link_libraries(EngineEditorUi PUBLIC EngineUiCore EngineUiWidgets EngineCore PRIVATE user32 imm32)
# G15：色彩 LUT 的 CPU 实现同时被后处理（EngineSandboxRenderer）与编辑器 LUT 面板使用，
# 单独成库避免同一符号被两个静态库重复链接。
add_library(EngineColorGrading STATIC Source/FunctionLayer/Rendering/ColorGrading.cpp Source/FunctionLayer/Rendering/ColorLutFile.cpp)
se_target(EngineColorGrading)
target_include_directories(EngineColorGrading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(EngineColorGrading PUBLIC EngineCore)
add_library(EngineEnvironmentAssets STATIC Source/ResourceLayer/Environment/TODProfile.cpp)
se_target(EngineEnvironmentAssets)
target_link_libraries(EngineEnvironmentAssets PUBLIC EngineCore)
add_library(EngineRuntime STATIC Source/FunctionLayer/Runtime/World.cpp Source/FunctionLayer/Runtime/WorldComponents.cpp Source/FunctionLayer/Runtime/Environment.cpp Source/ResourceLayer/Scenes/SceneSerializer.cpp Source/FunctionLayer/Runtime/ComponentMetadata.cpp Source/FunctionLayer/Runtime/Camera.cpp)
target_sources(EngineRuntime PRIVATE Source/FunctionLayer/Runtime/TickScheduler.cpp)
target_sources(EngineRuntime PRIVATE Source/FunctionLayer/Runtime/EventBus.cpp)
target_sources(EngineRuntime PRIVATE Source/FunctionLayer/Runtime/WorldSceneCommit.cpp)
target_sources(EngineRuntime PRIVATE Source/FunctionLayer/Runtime/SceneManager.cpp)
target_sources(EngineRuntime PRIVATE Source/FunctionLayer/Runtime/SceneSpatialIndex.cpp)
target_sources(EngineRuntime PRIVATE Source/FunctionLayer/Runtime/TODSystem.cpp)
se_target(EngineRuntime)
target_link_libraries(EngineRuntime PUBLIC EngineCore EngineEnvironmentAssets)
add_library(EnginePhysics STATIC Source/FunctionLayer/Physics/PhysicsWorld.cpp Source/FunctionLayer/Physics/CharacterController.cpp)
se_target(EnginePhysics)
target_link_libraries(EnginePhysics PUBLIC EngineCore)
add_library(ufbx STATIC ThirdParty/ufbx/ufbx.c)
set_target_properties(ufbx PROPERTIES C_STANDARD 99)
target_include_directories(ufbx PUBLIC ThirdParty/ufbx)
add_library(EngineAssets STATIC Source/ResourceLayer/Assets/AssetIO.cpp Source/ResourceLayer/Assets/ModelImporter.cpp Source/ResourceLayer/Assets/AssetStore.cpp Source/ResourceLayer/Assets/MaterialDocument.cpp Source/ResourceLayer/Assets/AssetTasks.cpp Source/ResourceLayer/Assets/Primitives.cpp Source/ResourceLayer/Assets/HdrImage.cpp Source/ResourceLayer/Assets/EnvironmentBake.cpp Source/ResourceLayer/Assets/EnvironmentIO.cpp Source/ResourceLayer/Assets/ImageWriter.cpp Source/ResourceLayer/Assets/VertexCompression.cpp Source/ResourceLayer/Assets/TextureCompression.cpp Source/ResourceLayer/Assets/Cgltf.c)
se_target(EngineAssets)
target_sources(EngineAssets PRIVATE Source/ResourceLayer/Assets/AsyncAssetLoader.cpp)
target_sources(EngineAssets PRIVATE Source/ResourceLayer/Assets/StaticModelPreparation.cpp)
target_sources(EngineAssets PRIVATE Source/ResourceLayer/Assets/MaterialMetadata.cpp)
target_sources(EngineAssets PRIVATE Source/ResourceLayer/Assets/TextureBake.cpp)
target_sources(EngineAssets PRIVATE Source/ResourceLayer/Assets/LodBake.cpp)
target_sources(EngineAssets PRIVATE Source/ResourceLayer/Assets/HlodBake.cpp)
target_sources(EngineAssets PRIVATE Source/ResourceLayer/Assets/MeshletBake.cpp)
target_sources(EngineAssets PRIVATE Source/ResourceLayer/Assets/ImpostorBake.cpp)
target_link_libraries(EngineAssets PUBLIC EngineCore EngineEnvironmentAssets PRIVATE ufbx bcrypt ole32 windowscodecs)
add_library(EngineGameUI STATIC Source/FunctionLayer/GameUI/UiTree.cpp Source/ResourceLayer/Fonts/FontCache.cpp Source/FunctionLayer/GameUI/ControlsGallery.cpp)
target_sources(EngineGameUI PRIVATE Source/FunctionLayer/GameUI/UiEventSystem.cpp)
target_sources(EngineGameUI PRIVATE Source/FunctionLayer/GameUI/UiTextPresentation.cpp)
target_sources(EngineGameUI PRIVATE Source/FunctionLayer/GameUI/UiControlGeometry.cpp)
target_sources(EngineGameUI PRIVATE Source/FunctionLayer/GameUI/UiTransitionDemo.cpp)
se_target(EngineGameUI)
target_link_libraries(EngineGameUI PUBLIC EngineCore PRIVATE dwrite)
add_library(imgui STATIC ThirdParty/imgui/imgui.cpp ThirdParty/imgui/imgui_draw.cpp ThirdParty/imgui/imgui_tables.cpp ThirdParty/imgui/imgui_widgets.cpp ThirdParty/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ThirdParty/imgui)
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32)
target_link_libraries(imgui PRIVATE user32 gdi32 dwmapi imm32)
add_library(EngineEditor STATIC Source/ToolLayer/Editor/MaterialThumbnail.cpp Source/ToolLayer/Editor/ExternalChangeMonitor.cpp Source/ToolLayer/Editor/BuildRestartRequest.cpp Source/ToolLayer/Editor/RecoveryApply.cpp Source/ToolLayer/Editor/EditorSession.cpp Source/ToolLayer/Editor/ImGuiEditor.cpp Source/ToolLayer/Editor/WindowsEditorInput.cpp Source/ToolLayer/Editor/EditorLayout.cpp Source/ToolLayer/Editor/EditorCommand.cpp Source/ToolLayer/Editor/AssetImportService.cpp Source/ToolLayer/Editor/EditorPlayback.cpp Source/ToolLayer/Editor/EditorSessionStore.cpp Source/ToolLayer/Editor/ExportOperation.cpp Source/ToolLayer/Editor/ProjectSettingsService.cpp Source/ToolLayer/Editor/ExportService.cpp Source/ToolLayer/Editor/ExportJob.cpp Source/ToolLayer/Editor/EditorProcessRunner.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/TODPreview.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/ExternalProcessBroker.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/ImGuiEditorRecovery.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/ImGuiEditorBuildControl.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/EditorAssetActions.cpp Source/ToolLayer/Editor/EditorScriptTools.cpp Source/ToolLayer/Editor/ImGuiEditorAssets.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/EditorSettingsDocuments.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/ImGuiGeometry.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/EditorViewportCamera.cpp Source/ToolLayer/Editor/EditorGizmo.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/PropertyWidgets.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/MaterialEditSession.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/SceneFileWorkflow.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/EditorPanelRegistry.cpp Source/ToolLayer/Editor/SceneDefaults.cpp)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/EditorTheme.cpp Source/ToolLayer/Editor/EditorDockLayout.cpp Source/ToolLayer/Editor/ImGuiEditorDocking.cpp)
se_target(EngineEditor)
target_link_libraries(EngineEditor PUBLIC EngineRuntime EngineAssets EngineGameUI imgui EnginePlatform EngineProjectSession EngineColorGrading)
target_link_libraries(EngineEditor PRIVATE comdlg32 shell32)
target_sources(EngineEditor PRIVATE Source/ToolLayer/Editor/UICanvasEditor.cpp)
add_library(EngineNative SHARED Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp Source/FunctionLayer/Scripting/NativeBridge/WindowProfile.cpp)
se_target(EngineNative)
target_link_libraries(EngineNative PRIVATE EngineRuntime EnginePhysics EngineGameUI EngineSandboxHost EngineSandboxRenderer EngineEditorApp EngineEditorRenderer)
add_library(EngineApplication STATIC Source/FunctionLayer/Application/FrameData.cpp Source/FunctionLayer/Application/FreeCameraInput.cpp)
se_target(EngineApplication)
target_link_libraries(EngineApplication PUBLIC EngineRuntime EnginePhysics EngineGameUI EngineAssets)
add_library(EnginePlatform STATIC Source/PlatformLayer/Windows/ExecutablePath.cpp
    Source/PlatformLayer/Windows/StartupProgress.cpp Source/PlatformLayer/Windows/AppBranding.cpp
    Source/PlatformLayer/Windows/ProcessDiagnostics.cpp
    Source/PlatformLayer/Windows/HResult.cpp Source/PlatformLayer/Windows/Window.cpp
    Source/PlatformLayer/Windows/WindowsInput.cpp Source/PlatformLayer/Windows/WindowsUiInput.cpp Source/PlatformLayer/Windows/FrameTimer.cpp
    Source/PlatformLayer/Windows/UiDirectWriteTextService.cpp
    Source/PlatformLayer/Windows/Win32UiPlatformServices.cpp)
se_target(EnginePlatform)
target_link_libraries(EnginePlatform PUBLIC EngineCore EngineGameUI PRIVATE user32 imm32 shell32)
add_library(EngineSandboxHost STATIC Source/FunctionLayer/Application/SandboxApp.cpp Source/FunctionLayer/Application/SandboxDiagnostics.cpp)
se_target(EngineSandboxHost)
target_link_libraries(EngineSandboxHost PUBLIC EnginePlatform EngineApplication)
add_library(EngineEditorApp STATIC Source/ToolLayer/Editor/EditorApplication.cpp Source/ToolLayer/Editor/EditorPlayInput.cpp Source/ToolLayer/Editor/EditorViewportController.cpp Source/ToolLayer/Editor/EditorFloatingWindows.cpp)
se_target(EngineEditorApp)
target_link_libraries(EngineEditorApp PUBLIC EngineEditor EngineSandboxHost EngineEditorUi EngineUiRender EnginePlatform EngineSandboxRenderer PRIVATE dwmapi)
add_library(EngineProject STATIC Source/ToolLayer/Tools/Project.cpp)
se_target(EngineProject)
target_link_libraries(EngineProject PUBLIC EngineRuntime EngineAssets EngineGameUI EnginePhysics EngineCore)
add_library(EngineProjectSession STATIC Source/ToolLayer/Editor/ProjectWorkspaceSession.cpp)
se_target(EngineProjectSession)
target_link_libraries(EngineProjectSession PUBLIC EngineProject EnginePlatform)
add_executable(ProjectTool Source/ToolLayer/Tools/ProjectTool.cpp)
se_target(ProjectTool)
target_link_libraries(ProjectTool PRIVATE EngineProject)
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
add_library(EngineSandboxRenderer STATIC Source/FunctionLayer/Rendering/SandboxRenderer.cpp Source/FunctionLayer/Rendering/StaticModelRender.cpp Source/FunctionLayer/Rendering/Visibility.cpp Source/FunctionLayer/Rendering/RenderQueue.cpp Source/FunctionLayer/Rendering/Instancing.cpp Source/FunctionLayer/Rendering/ForwardRenderer.cpp Source/FunctionLayer/Rendering/ShadowSystem.cpp Source/FunctionLayer/Rendering/CascadeShadow.cpp Source/FunctionLayer/Rendering/LightClustering.cpp Source/FunctionLayer/Rendering/AmbientOcclusion.cpp Source/FunctionLayer/Rendering/Atmosphere.cpp Source/FunctionLayer/Rendering/Reflection.cpp Source/FunctionLayer/Rendering/FrameGraph.cpp Source/FunctionLayer/Rendering/TemporalAA.cpp Source/FunctionLayer/Rendering/AutoExposure.cpp Source/FunctionLayer/Rendering/EnvironmentRender.cpp Source/FunctionLayer/Rendering/PostProcessRenderer.cpp)
se_target(EngineSandboxRenderer)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/AssetUploadScheduler.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/GuiRenderer.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/EditorViewportTarget.cpp Source/FunctionLayer/Rendering/GuiCompositor.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/GameUIRenderer.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/OcclusionHud.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/PhysicsDebugDraw.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/RenderTelemetry.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/PipelineWarmup.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/TextureResidency.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/LodSelect.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/HlodSelect.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/HlodStream.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/HlodStreamer.cpp Source/FunctionLayer/Rendering/HlodProxyCache.cpp)
target_sources(EngineSandboxRenderer PRIVATE Source/FunctionLayer/Rendering/HiZ.cpp)
target_link_libraries(EngineSandboxRenderer PUBLIC EnginePlatform EngineRuntime EngineGraphics EngineColorGrading PRIVATE EngineAssets)
add_library(EngineEditorProduct STATIC Source/ToolLayer/Editor/EditorLauncher.cpp Source/ToolLayer/Editor/LauncherStyle.cpp Source/ToolLayer/Editor/EditorWorker.cpp Source/ToolLayer/Editor/EditorLoadingScreen.cpp)
target_sources(EngineEditorProduct PRIVATE Source/ToolLayer/Editor/LoadingRibbon.cpp)
se_target(EngineEditorProduct)
add_library(EngineEditorRenderer STATIC Source/ToolLayer/Editor/EditorRenderer.cpp)
se_target(EngineEditorRenderer)
target_link_libraries(EngineEditorRenderer PUBLIC EngineSandboxRenderer)
target_link_libraries(EngineEditorProduct PUBLIC EngineEditorApp EngineEditorRenderer EngineSandboxRenderer EngineAssets EngineProjectSession ole32 uuid shell32 comdlg32 gdi32 PRIVATE dwmapi uxtheme comctl32 gdiplus)
add_executable(SelfEngineEditor WIN32 Source/ToolLayer/Tools/SelfEngineEditor.cpp)
se_target(SelfEngineEditor)
target_link_libraries(SelfEngineEditor PRIVATE EngineEditorProduct)
set_target_properties(SelfEngineEditor PROPERTIES OUTPUT_NAME FlareEditor)
foreach(flare_target SelfEngineEditor EngineNative)
    target_sources(${flare_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/FlareResources.rc")
endforeach()
target_compile_definitions(EngineNative PRIVATE FLARE_NATIVE_LIBRARY)
# 旧快捷方式/脚本仍能启动同一个 Flare 程序；正式入口和新打包均使用 FlareEditor.exe。
add_custom_command(TARGET SelfEngineEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SelfEngineEditor>" "$<TARGET_FILE_DIR:SelfEngineEditor>/GearEditor.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SelfEngineEditor>" "$<TARGET_FILE_DIR:SelfEngineEditor>/SelfEngineEditor.exe" VERBATIM)
add_custom_target(CopyLocalizationResources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SelfEngineEditor>/Resources/Localization"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Localization/zh-CN.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Localization/en-US.txt"
        "$<TARGET_FILE_DIR:SelfEngineEditor>/Resources/Localization" VERBATIM)
add_dependencies(SelfEngineEditor CopyLocalizationResources CopyRendererResources)
add_custom_target(CopyFlareBranding ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Resources/Branding"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Branding/Flare.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Branding/FlareMark.png"
        "${CMAKE_BINARY_DIR}/Resources/Branding" VERBATIM)
add_dependencies(SelfEngineEditor CopyFlareBranding)
add_custom_target(CopyEditorTemplates ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Templates"
        "${CMAKE_BINARY_DIR}/Resources/Templates" VERBATIM)
add_dependencies(SelfEngineEditor CopyEditorTemplates)
add_dependencies(EngineNative CopyLocalizationResources)
add_custom_target(CopyRendererResources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SelfEngineEditor>/Resources/Shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders"
        "$<TARGET_FILE_DIR:SelfEngineEditor>/Resources/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SelfEngineEditor>/Resources/Textures"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Textures"
        "$<TARGET_FILE_DIR:SelfEngineEditor>/Resources/Textures" VERBATIM)
add_dependencies(EngineNative CopyRendererResources)
add_executable(ShaderBuilder Source/ToolLayer/Tools/ShaderBuilder.cpp)
se_target(ShaderBuilder)
target_link_libraries(ShaderBuilder PRIVATE EngineGraphics EnginePlatform)
add_dependencies(ShaderBuilder CopyLocalizationResources)
add_executable(EnvironmentCooker Source/ToolLayer/Tools/EnvironmentCooker.cpp)
se_target(EnvironmentCooker)
target_link_libraries(EnvironmentCooker PRIVATE EngineAssets EnginePlatform)
add_dependencies(EnvironmentCooker CopyLocalizationResources)
set(environment_outputs "${CMAKE_BINARY_DIR}/Resources/Environments")
foreach(environment_kind soft contrast)
    add_custom_command(OUTPUT "${environment_outputs}/${environment_kind}.stamp"
        COMMAND EnvironmentCooker sample ${environment_kind} "${environment_outputs}/${environment_kind}.hdr"
        COMMAND EnvironmentCooker cook "${environment_outputs}/${environment_kind}.hdr" "${environment_outputs}/${environment_kind}.seenv"
        COMMAND ${CMAKE_COMMAND} -E touch "${environment_outputs}/${environment_kind}.stamp"
        DEPENDS EnvironmentCooker
        BYPRODUCTS "${environment_outputs}/${environment_kind}.hdr" "${environment_outputs}/${environment_kind}.seenv"
            "${environment_outputs}/${environment_kind}.seenv.json" "${environment_outputs}/${environment_kind}.seenv.asset.json" VERBATIM)
endforeach()
add_custom_target(BuildEnvironments ALL DEPENDS "${environment_outputs}/soft.stamp" "${environment_outputs}/contrast.stamp")
add_dependencies(CopyRendererResources BuildEnvironments)
set(shader_artifacts "${CMAKE_BINARY_DIR}/Resources/Shaders")
add_custom_command(OUTPUT "${shader_artifacts}/Triangle.hlsl.sebin" "${shader_artifacts}/Triangle.hlsl.sebin.json"
    COMMAND ShaderBuilder triangle "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Triangle.hlsl" "${shader_artifacts}/Triangle.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Triangle.hlsl" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/TexturedMesh.hlsl.sebin" "${shader_artifacts}/TexturedMesh.hlsl.sebin.json"
    COMMAND ShaderBuilder mesh "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/TexturedMesh.hlsl" "${shader_artifacts}/TexturedMesh.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/TexturedMesh.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/MeshBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/Pbr.hlsl.sebin" "${shader_artifacts}/Pbr.hlsl.sebin.json"
    COMMAND ShaderBuilder pbr "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${shader_artifacts}/Pbr.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/PbrInstanced.hlsl.sebin" "${shader_artifacts}/PbrInstanced.hlsl.sebin.json"
    COMMAND ShaderBuilder pbr-instanced "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrInstanced.hlsl" "${shader_artifacts}/PbrInstanced.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrInstanced.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/InstancingBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/PbrForwardInstanced.hlsl.sebin" "${shader_artifacts}/PbrForwardInstanced.hlsl.sebin.json"
    COMMAND ShaderBuilder pbr-forward-instanced "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrForwardInstanced.hlsl" "${shader_artifacts}/PbrForwardInstanced.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrForwardInstanced.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/InstancingBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/Forward.hlsl.sebin" "${shader_artifacts}/Forward.hlsl.sebin.json"
    COMMAND ShaderBuilder forward "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${shader_artifacts}/Forward.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/FinalOutput.hlsl.sebin" "${shader_artifacts}/FinalOutput.hlsl.sebin.json"
    COMMAND ShaderBuilder output "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/FinalOutput.hlsl" "${shader_artifacts}/FinalOutput.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/FinalOutput.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Fullscreen.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/Sky.hlsl.sebin" "${shader_artifacts}/Sky.hlsl.sebin.json"
    COMMAND ShaderBuilder sky "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Sky.hlsl" "${shader_artifacts}/Sky.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Sky.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Fullscreen.hlsli" VERBATIM)
add_custom_target(BuildShaders ALL DEPENDS "${shader_artifacts}/Triangle.hlsl.sebin" "${shader_artifacts}/TexturedMesh.hlsl.sebin" "${shader_artifacts}/Pbr.hlsl.sebin" "${shader_artifacts}/PbrInstanced.hlsl.sebin"
    "${shader_artifacts}/PbrForwardInstanced.hlsl.sebin"
    "${shader_artifacts}/Forward.hlsl.sebin" "${shader_artifacts}/FinalOutput.hlsl.sebin" "${shader_artifacts}/Sky.hlsl.sebin"
    "${shader_artifacts}/ShadowedForward.hlsl.sebin" "${shader_artifacts}/ShadowDepth.hlsl.sebin" "${shader_artifacts}/ShadowOutput.hlsl.sebin"
    "${shader_artifacts}/BasicLit.hlsl.sebin"
    "${shader_artifacts}/EnvironmentForward.hlsl.sebin" "${shader_artifacts}/EnvironmentSky.hlsl.sebin"
    "${shader_artifacts}/BloomExtract.hlsl.sebin" "${shader_artifacts}/BloomBlur.hlsl.sebin" "${shader_artifacts}/ToneMap.hlsl.sebin"
    "${shader_artifacts}/Fxaa.hlsl.sebin" "${shader_artifacts}/UiOverlay.hlsl.sebin")
add_custom_command(OUTPUT "${shader_artifacts}/ShadowedForward.hlsl.sebin" "${shader_artifacts}/ShadowedForward.hlsl.sebin.json"
    COMMAND ShaderBuilder shadowed "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl" "${shader_artifacts}/ShadowedForward.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/BasicLit.hlsl.sebin" "${shader_artifacts}/BasicLit.hlsl.sebin.json"
    COMMAND ShaderBuilder basiclit "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl" "${shader_artifacts}/BasicLit.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/ShadowDepth.hlsl.sebin" "${shader_artifacts}/ShadowDepth.hlsl.sebin.json"
    COMMAND ShaderBuilder shadowdepth "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowDepth.hlsl" "${shader_artifacts}/ShadowDepth.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowDepth.hlsl" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/ShadowOutput.hlsl.sebin" "${shader_artifacts}/ShadowOutput.hlsl.sebin.json"
    COMMAND ShaderBuilder shadowoutput "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowOutput.hlsl" "${shader_artifacts}/ShadowOutput.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowOutput.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/FinalOutput.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Fullscreen.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/EnvironmentForward.hlsl.sebin" "${shader_artifacts}/EnvironmentForward.hlsl.sebin.json"
    COMMAND ShaderBuilder environment "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentForward.hlsl" "${shader_artifacts}/EnvironmentForward.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentForward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/EnvironmentSky.hlsl.sebin" "${shader_artifacts}/EnvironmentSky.hlsl.sebin.json"
    COMMAND ShaderBuilder environmentsky "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentSky.hlsl" "${shader_artifacts}/EnvironmentSky.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentSky.hlsl" VERBATIM)
add_dependencies(CopyRendererResources BuildShaders)
add_custom_command(OUTPUT "${shader_artifacts}/Gui.hlsl.sebin" "${shader_artifacts}/Gui.hlsl.sebin.json"
    COMMAND ShaderBuilder gui "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Gui.hlsl" "${shader_artifacts}/Gui.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Gui.hlsl" VERBATIM)
add_custom_target(CopyGuiResources
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Gui.hlsl" "${shader_artifacts}/Gui.hlsl"
    DEPENDS "${shader_artifacts}/Gui.hlsl.sebin" VERBATIM)
add_dependencies(CopyRendererResources CopyGuiResources)
foreach(post_shader BloomExtract BloomBlur ToneMap Fxaa UiOverlay)
    string(TOLOWER "${post_shader}" post_kind)
    if(post_shader STREQUAL "UiOverlay")
        set(post_kind overlay)
    endif()
    add_custom_command(OUTPUT "${shader_artifacts}/${post_shader}.hlsl.sebin" "${shader_artifacts}/${post_shader}.hlsl.sebin.json"
        COMMAND ShaderBuilder ${post_kind} "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${post_shader}.hlsl" "${shader_artifacts}/${post_shader}.hlsl.sebin"
        DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${post_shader}.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PostBindings.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Fullscreen.hlsli" VERBATIM)
endforeach()

# 本地开发扩展不属于公开发布内容。
include("${CMAKE_CURRENT_SOURCE_DIR}/Tests/LocalBuild.cmake" OPTIONAL)
