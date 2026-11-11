// 公开 API 消费者：只 include 冻结的公开头，不 include 任何内部/平台头。
// 如果公开头泄漏 Windows.h、d3d12.h 或 imgui.h，下面的预处理器检查会直接失败。
#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiElement.h"
#include "FunctionLayer/UI/Core/UiFrameData.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"
#include "FunctionLayer/UI/Interfaces/UiImageProvider.h"
#include "FunctionLayer/UI/Interfaces/UiPaintSink.h"
#include "FunctionLayer/UI/Interfaces/UiPlatformServices.h"
#include "FunctionLayer/UI/Interfaces/UiTextService.h"

#if defined(_WINDOWS_)
#error "public UI header leaked Windows.h"
#endif
#if defined(__d3d12_h__)
#error "public UI header leaked d3d12.h"
#endif
#if defined(IMGUI_VERSION)
#error "public UI header leaked imgui.h"
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        se::ui::UiDocument document;
        const se::ui::UiElement element(se::ui::UiElementHandle{1, 1});
        Check(document.IsValid(), "consumer can construct a document from public headers");
        Check(element.IsValid(), "consumer can construct an element from public headers");

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"publicHeadersOnly\":true,\"windowsLeak\":false,"
                   "\"d3d12Leak\":false,\"imguiLeak\":false}\n";
        }
        std::cout << "UiApiConsumerTests passed (public headers only; no platform/ImGui types).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
