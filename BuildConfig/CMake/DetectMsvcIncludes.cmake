# cl 使用输出代码页；它可能与 CMake AUTO 采用的输入代码页不同。
# 先以同一进程环境探测输出代码页，再显式解码为 CMake 的 UTF-8。
# NONE 会把后台 ANSI 输出误当 UTF-8，甚至生成不完整的 rules.ninja。
if(MSVC AND CMAKE_GENERATOR MATCHES "^Ninja")
    set(_se_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/selfengine-msvc-includes")
    file(MAKE_DIRECTORY "${_se_probe_dir}")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /nologo /MD /utf-8
        "${CMAKE_CURRENT_LIST_DIR}/MsvcOutputEncoding.cpp"
        "/Fo${_se_probe_dir}/encoding.obj" "/Fe${_se_probe_dir}/encoding.exe" /link kernel32.lib
        RESULT_VARIABLE _se_encoding_build OUTPUT_QUIET ERROR_QUIET)
    if(NOT _se_encoding_build EQUAL 0)
        message(FATAL_ERROR "Cannot compile MSVC output encoding probe: ${_se_encoding_build}")
    endif()
    execute_process(COMMAND "${_se_probe_dir}/encoding.exe"
        RESULT_VARIABLE _se_encoding_status OUTPUT_VARIABLE _se_output_encoding
        OUTPUT_STRIP_TRAILING_WHITESPACE ENCODING UTF-8)
    if(NOT _se_encoding_status EQUAL 0 OR NOT _se_output_encoding MATCHES "^(UTF-8|ANSI|OEM)$")
        message(FATAL_ERROR "Cannot determine MSVC output encoding: ${_se_output_encoding}")
    endif()
    file(WRITE "${_se_probe_dir}/encoding.txt" "${_se_output_encoding}\n")
    file(WRITE "${_se_probe_dir}/selfengine_msvc_probe.h" "#pragma once\n")
    file(WRITE "${_se_probe_dir}/probe.cpp" "#include \"selfengine_msvc_probe.h\"\nint selfengine_msvc_probe = 0;\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /c /showIncludes /utf-8
            "${_se_probe_dir}/probe.cpp" "/Fo${_se_probe_dir}/probe.obj"
        RESULT_VARIABLE _se_probe_status
        OUTPUT_VARIABLE _se_probe_stdout
        ERROR_VARIABLE _se_probe_stderr
        ENCODING ${_se_output_encoding})
    # 此处保存解码后的 UTF-8，prefix.hex 同样是内部 UTF-8 的字节。
    file(WRITE "${_se_probe_dir}/stdout.utf8.txt" "${_se_probe_stdout}")
    file(WRITE "${_se_probe_dir}/stderr.utf8.txt" "${_se_probe_stderr}")
    if(NOT _se_probe_status EQUAL 0)
        message(FATAL_ERROR "MSVC /showIncludes probe failed: ${_se_probe_status}; see ${_se_probe_dir}")
    endif()
    string(REPLACE "\r" "" _se_probe_output "${_se_probe_stdout}\n${_se_probe_stderr}")
    string(REGEX MATCH "(^|\n)([^\n]*:)[ \t]+[^\n]*selfengine_msvc_probe[.]h" _se_probe_match "${_se_probe_output}")
    if(NOT _se_probe_match)
        message(FATAL_ERROR "Cannot identify MSVC include prefix; see ${_se_probe_dir}/stdout.utf8.txt")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    string(HEX "${CMAKE_CL_SHOWINCLUDES_PREFIX}" _se_prefix_hex)
    file(WRITE "${_se_probe_dir}/prefix.hex" "${_se_prefix_hex}\n")
    message(STATUS "SelfEngine: decoded MSVC include prefix using its console code page")
    # SelfEngine 也可以作为模板工程的子目录，父工程中的 Main.cpp 使用同一编译器。
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_CL_SHOWINCLUDES_PREFIX}" PARENT_SCOPE)
    endif()
endif()
