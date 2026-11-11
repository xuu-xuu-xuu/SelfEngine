#pragma once

#include "CoreLayer/Log.h"
#include <Windows.h>
#include <string_view>

class Localization;

// 图形诊断使用已加载的语言表；原有 T02 HRESULT 练习暂不迁移。
bool CheckGraphicsResult(HRESULT result, const Localization& localization,
    std::string_view errorKey, LogLevel level = LogLevel::Error);
