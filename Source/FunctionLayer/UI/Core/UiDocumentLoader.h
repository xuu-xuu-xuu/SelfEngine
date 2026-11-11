#pragma once

#include "FunctionLayer/UI/Core/UiDocument.h"

#include <string>
#include <string_view>

namespace se::ui
{

struct UiDocumentLoadResult
{
    bool success = false;
    std::string error;
};

// 解析 JSON v1（version/root/styles）并替换 document。失败时 document 保持原样。
// 当前只解析树、id 和 classes；styles 只做对象形状校验，不接级联。
[[nodiscard]] UiDocumentLoadResult LoadUiDocumentJson(std::string_view json, UiDocument& document);

} // namespace se::ui
