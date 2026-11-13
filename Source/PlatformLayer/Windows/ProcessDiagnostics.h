#pragma once
// 重复调用不打开新文件；进程作用域保证晚于引擎资源销毁记录退出。
void InitializeProcessDiagnostics(const char* role) noexcept;
