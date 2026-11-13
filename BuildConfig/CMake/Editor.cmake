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

add_library(imgui STATIC
    ThirdParty/imgui/imgui.cpp
    ThirdParty/imgui/imgui_draw.cpp
    ThirdParty/imgui/imgui_tables.cpp
    ThirdParty/imgui/imgui_widgets.cpp
    ThirdParty/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ThirdParty/imgui)
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32)
target_link_libraries(imgui PRIVATE user32 gdi32 dwmapi imm32)

add_library(EngineEditor STATIC
    Source/ToolLayer/Editor/MaterialThumbnail.cpp
    Source/ToolLayer/Editor/ExternalChangeMonitor.cpp
    Source/ToolLayer/Editor/BuildRestartRequest.cpp
    Source/ToolLayer/Editor/RecoveryApply.cpp
    Source/ToolLayer/Editor/EditorSession.cpp
    Source/ToolLayer/Editor/ImGuiEditor.cpp
    Source/ToolLayer/Editor/WindowsEditorInput.cpp
    Source/ToolLayer/Editor/EditorLayout.cpp
    Source/ToolLayer/Editor/EditorCommand.cpp
    Source/ToolLayer/Editor/AssetImportService.cpp
    Source/ToolLayer/Editor/EditorPlayback.cpp
    Source/ToolLayer/Editor/EditorSessionStore.cpp
    Source/ToolLayer/Editor/ExportOperation.cpp
    Source/ToolLayer/Editor/ProjectSettingsService.cpp
    Source/ToolLayer/Editor/ExportService.cpp
    Source/ToolLayer/Editor/ExportJob.cpp
    Source/ToolLayer/Editor/EditorProcessRunner.cpp
    Source/ToolLayer/Editor/TODPreview.cpp
    Source/ToolLayer/Editor/ExternalProcessBroker.cpp
    Source/ToolLayer/Editor/ImGuiEditorRecovery.cpp
    Source/ToolLayer/Editor/ImGuiEditorBuildControl.cpp
    Source/ToolLayer/Editor/EditorAssetActions.cpp
    Source/ToolLayer/Editor/EditorScriptTools.cpp
    Source/ToolLayer/Editor/ImGuiEditorAssets.cpp
    Source/ToolLayer/Editor/EditorSettingsDocuments.cpp
    Source/ToolLayer/Editor/ImGuiGeometry.cpp
    Source/ToolLayer/Editor/EditorViewportCamera.cpp
    Source/ToolLayer/Editor/EditorGizmo.cpp
    Source/ToolLayer/Editor/PropertyWidgets.cpp
    Source/ToolLayer/Editor/MaterialEditSession.cpp
    Source/ToolLayer/Editor/SceneFileWorkflow.cpp
    Source/ToolLayer/Editor/EditorPanelRegistry.cpp
    Source/ToolLayer/Editor/SceneDefaults.cpp
    Source/ToolLayer/Editor/EditorTheme.cpp
    Source/ToolLayer/Editor/EditorDockLayout.cpp
    Source/ToolLayer/Editor/ImGuiEditorDocking.cpp
    Source/ToolLayer/Editor/UICanvasEditor.cpp)
se_target(EngineEditor)
target_link_libraries(EngineEditor PUBLIC EngineRuntime EngineAssets EngineGameUI imgui EnginePlatform EngineProjectSession EngineColorGrading)
target_link_libraries(EngineEditor PRIVATE comdlg32 shell32)

add_library(EngineEditorApp STATIC
    Source/ToolLayer/Editor/EditorApplication.cpp
    Source/ToolLayer/Editor/EditorPlayInput.cpp
    Source/ToolLayer/Editor/EditorViewportController.cpp
    Source/ToolLayer/Editor/EditorFloatingWindows.cpp)
se_target(EngineEditorApp)
target_link_libraries(EngineEditorApp PUBLIC EngineEditor EngineSandboxHost EngineEditorUi EngineUiRender EnginePlatform EngineSandboxRenderer PRIVATE dwmapi)

add_library(EngineEditorProduct STATIC
    Source/ToolLayer/Editor/EditorLauncher.cpp
    Source/ToolLayer/Editor/LauncherStyle.cpp
    Source/ToolLayer/Editor/EditorWorker.cpp
    Source/ToolLayer/Editor/EditorLoadingScreen.cpp
    Source/ToolLayer/Editor/LoadingRibbon.cpp)
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
