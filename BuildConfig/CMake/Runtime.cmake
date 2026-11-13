add_library(EngineCore STATIC
    Source/CoreLayer/Log.cpp
    Source/CoreLayer/Localization.cpp
    Source/CoreLayer/Utf.cpp
    Source/CoreLayer/TaskQueue.cpp
    Source/CoreLayer/Profiler.cpp
    Source/CoreLayer/Hash.cpp
    Source/CoreLayer/Spatial/BoundsIndex.cpp)
se_target(EngineCore)
target_link_libraries(EngineCore PRIVATE bcrypt)
# UI 核心只依赖 EngineCore。

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

# 色彩 LUT 的 CPU 实现同时被后处理（EngineSandboxRenderer）与编辑器 LUT 面板使用，
# 单独成库避免同一符号被两个静态库重复链接。

add_library(EngineColorGrading STATIC Source/FunctionLayer/Rendering/ColorGrading.cpp Source/FunctionLayer/Rendering/ColorLutFile.cpp)
se_target(EngineColorGrading)
target_include_directories(EngineColorGrading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(EngineColorGrading PUBLIC EngineCore)

add_library(EngineEnvironmentAssets STATIC Source/ResourceLayer/Environment/TODProfile.cpp)
se_target(EngineEnvironmentAssets)
target_link_libraries(EngineEnvironmentAssets PUBLIC EngineCore)

add_library(EngineRuntime STATIC
    Source/FunctionLayer/Runtime/World.cpp
    Source/FunctionLayer/Runtime/WorldComponents.cpp
    Source/FunctionLayer/Runtime/Environment.cpp
    Source/ResourceLayer/Scenes/SceneSerializer.cpp
    Source/FunctionLayer/Runtime/ComponentMetadata.cpp
    Source/FunctionLayer/Runtime/Camera.cpp
    Source/FunctionLayer/Runtime/TickScheduler.cpp
    Source/FunctionLayer/Runtime/EventBus.cpp
    Source/FunctionLayer/Runtime/WorldSceneCommit.cpp
    Source/FunctionLayer/Runtime/SceneManager.cpp
    Source/FunctionLayer/Runtime/SceneSpatialIndex.cpp
    Source/FunctionLayer/Runtime/TODSystem.cpp)
se_target(EngineRuntime)
target_link_libraries(EngineRuntime PUBLIC EngineCore EngineEnvironmentAssets)

add_library(EnginePhysics STATIC Source/FunctionLayer/Physics/PhysicsWorld.cpp Source/FunctionLayer/Physics/CharacterController.cpp)
se_target(EnginePhysics)
target_link_libraries(EnginePhysics PUBLIC EngineCore)

add_library(ufbx STATIC ThirdParty/ufbx/ufbx.c)
set_target_properties(ufbx PROPERTIES C_STANDARD 99)
target_include_directories(ufbx PUBLIC ThirdParty/ufbx)

add_library(EngineAssets STATIC
    Source/ResourceLayer/Assets/AssetIO.cpp
    Source/ResourceLayer/Assets/ModelImporter.cpp
    Source/ResourceLayer/Assets/AssetStore.cpp
    Source/ResourceLayer/Assets/MaterialDocument.cpp
    Source/ResourceLayer/Assets/AssetTasks.cpp
    Source/ResourceLayer/Assets/Primitives.cpp
    Source/ResourceLayer/Assets/HdrImage.cpp
    Source/ResourceLayer/Assets/EnvironmentBake.cpp
    Source/ResourceLayer/Assets/EnvironmentIO.cpp
    Source/ResourceLayer/Assets/ImageWriter.cpp
    Source/ResourceLayer/Assets/VertexCompression.cpp
    Source/ResourceLayer/Assets/TextureCompression.cpp
    Source/ResourceLayer/Assets/Cgltf.c
    Source/ResourceLayer/Assets/AsyncAssetLoader.cpp
    Source/ResourceLayer/Assets/StaticModelPreparation.cpp
    Source/ResourceLayer/Assets/MaterialMetadata.cpp
    Source/ResourceLayer/Assets/TextureBake.cpp
    Source/ResourceLayer/Assets/LodBake.cpp
    Source/ResourceLayer/Assets/HlodBake.cpp
    Source/ResourceLayer/Assets/MeshletBake.cpp
    Source/ResourceLayer/Assets/ImpostorBake.cpp)
se_target(EngineAssets)
target_link_libraries(EngineAssets PUBLIC EngineCore EngineEnvironmentAssets PRIVATE ufbx bcrypt ole32 windowscodecs)

add_library(EngineGameUI STATIC
    Source/FunctionLayer/GameUI/UiTree.cpp
    Source/ResourceLayer/Fonts/FontCache.cpp
    Source/FunctionLayer/GameUI/ControlsGallery.cpp
    Source/FunctionLayer/GameUI/UiEventSystem.cpp
    Source/FunctionLayer/GameUI/UiTextPresentation.cpp
    Source/FunctionLayer/GameUI/UiControlGeometry.cpp
    Source/FunctionLayer/GameUI/UiTransitionDemo.cpp)
se_target(EngineGameUI)
target_link_libraries(EngineGameUI PUBLIC EngineCore PRIVATE dwrite)

add_library(EngineNative SHARED Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp Source/FunctionLayer/Scripting/NativeBridge/WindowProfile.cpp)
se_target(EngineNative)
target_link_libraries(EngineNative PRIVATE EngineRuntime EnginePhysics EngineGameUI EngineSandboxHost EngineSandboxRenderer EngineEditorApp EngineEditorRenderer)

add_library(EngineApplication STATIC Source/FunctionLayer/Application/FrameData.cpp Source/FunctionLayer/Application/FreeCameraInput.cpp)
se_target(EngineApplication)
target_link_libraries(EngineApplication PUBLIC EngineRuntime EnginePhysics EngineGameUI EngineAssets)

add_library(EnginePlatform STATIC
    Source/PlatformLayer/Windows/ExecutablePath.cpp
    Source/PlatformLayer/Windows/StartupProgress.cpp
    Source/PlatformLayer/Windows/AppBranding.cpp
    Source/PlatformLayer/Windows/ProcessDiagnostics.cpp
    Source/PlatformLayer/Windows/HResult.cpp
    Source/PlatformLayer/Windows/Window.cpp
    Source/PlatformLayer/Windows/WindowsInput.cpp
    Source/PlatformLayer/Windows/WindowsUiInput.cpp
    Source/PlatformLayer/Windows/FrameTimer.cpp
    Source/PlatformLayer/Windows/UiDirectWriteTextService.cpp
    Source/PlatformLayer/Windows/Win32UiPlatformServices.cpp)
se_target(EnginePlatform)
target_link_libraries(EnginePlatform PUBLIC EngineCore EngineGameUI PRIVATE user32 imm32 shell32)

add_library(EngineSandboxHost STATIC Source/FunctionLayer/Application/SandboxApp.cpp Source/FunctionLayer/Application/SandboxDiagnostics.cpp)
se_target(EngineSandboxHost)
target_link_libraries(EngineSandboxHost PUBLIC EnginePlatform EngineApplication)

add_library(EngineProject STATIC Source/ToolLayer/Tools/Project.cpp)
se_target(EngineProject)
target_link_libraries(EngineProject PUBLIC EngineRuntime EngineAssets EngineGameUI EnginePhysics EngineCore)

add_library(EngineProjectSession STATIC Source/ToolLayer/Editor/ProjectWorkspaceSession.cpp)
se_target(EngineProjectSession)
target_link_libraries(EngineProjectSession PUBLIC EngineProject EnginePlatform)

add_executable(ProjectTool Source/ToolLayer/Tools/ProjectTool.cpp)
se_target(ProjectTool)
target_link_libraries(ProjectTool PRIVATE EngineProject)
