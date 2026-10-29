#pragma once

#include <filesystem>

// 从操作系统查询当前可执行文件的目录；失败时返回空路径。
std::filesystem::path GetExecutableDirectory();
