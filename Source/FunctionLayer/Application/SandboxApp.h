#pragma once
#include <string>

class Renderer;
namespace se { struct ApplicationServices; }

// 创建窗口、运行交互循环并清理；返回进程应采用的退出码。
// 默认显示窗口；隐藏选项供自动化运行验证使用，不改变消息泵路径。
int RunSandboxApp(const std::string& locale, Renderer& renderer, bool showWindow = true,
    se::ApplicationServices* services = nullptr);
