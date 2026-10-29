#pragma once
#include <string>

// 创建窗口、运行交互循环并清理；返回进程应采用的退出码。
int RunSandboxApp(const std::string& locale);
