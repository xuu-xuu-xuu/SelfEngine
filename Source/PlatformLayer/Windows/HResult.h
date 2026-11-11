#pragma once

#include<Windows.h>
#include<string>

// 检查结果 hr 并记录失败的 operation；返回 true 表示成功。
bool CheckHResult(HRESULT hr, const std::string& operation);
