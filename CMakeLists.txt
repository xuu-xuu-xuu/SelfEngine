cmake_minimum_required(VERSION 3.25)

project(SelfEngine VERSION 0.1.0 LANGUAGES CXX)

add_executable(EngineSandbox
                Source/Core/Localization.h
                Source/Core/Localization.cpp
                Source/Platform/Windows/ExecutablePath.h
                Source/Platform/Windows/ExecutablePath.cpp
				Source/Sandbox/main.cpp
				Source/Sandbox/SandboxApp.h
				Source/Sandbox/SandboxApp.cpp
				Source/Sandbox/SandboxDiagnostics.h
				Source/Sandbox/SandboxDiagnostics.cpp
				Source/Sandbox/T02Examples.h
				Source/Sandbox/T02Examples.cpp
				"Source/Core/Log.h" 
			    "Source/Core/Log.cpp"
				Source/Platform/Windows/HResult.h
				Source/Platform/Windows/HResult.cpp
				Source/Platform/Windows/Window.h
				Source/Platform/Windows/Window.cpp
				Source/Platform/InputState.h
				Source/Platform/InputEvents.h
				Source/Platform/Windows/WindowsInput.h
				Source/Platform/Windows/WindowsInput.cpp
				Source/Platform/Windows/FrameTimer.h
				Source/Platform/Windows/FrameTimer.cpp
)

target_include_directories(EngineSandbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(EngineSandbox PRIVATE d3dcompiler user32)

target_compile_features(EngineSandbox PRIVATE cxx_std_20)

# 每次构建同步语言表，单独修改文案也能生效；资源随可执行文件分发。
add_custom_target(CopyLocalizationResources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:EngineSandbox>/Resources/Localization"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Localization/zh-CN.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Localization/en-US.txt"
        "$<TARGET_FILE_DIR:EngineSandbox>/Resources/Localization"
    VERBATIM
)
add_dependencies(EngineSandbox CopyLocalizationResources)

set_target_properties(EngineSandbox PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
	target_compile_options(EngineSandbox PRIVATE 
							/W4
							/permissive-
							/utf-8)
endif()
