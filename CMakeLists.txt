cmake_minimum_required(VERSION 3.25)

project(SelfEngine VERSION 0.1.0 LANGUAGES CXX)

add_executable(EngineSandbox
				Source/Sandbox/main.cpp
				"Source/Core/Log.h" 
			    "Source/Core/Log.cpp"
				Source/Platform/Windows/HResult.h
				Source/Platform/Windows/HResult.cpp
)

 target_include_directories(EngineSandbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(EngineSandbox PRIVATE d3dcompiler)

target_compile_features(EngineSandbox PRIVATE cxx_std_20)

set_target_properties(EngineSandbox PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
	target_compile_options(EngineSandbox PRIVATE 
							/W4
							/permissive-
							/utf-8)
endif()