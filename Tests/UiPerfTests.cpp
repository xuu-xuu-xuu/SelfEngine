#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Widgets/UiWidgets.h"
#include "ToolLayer/Editor/EditorUi/EditorShell.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace se::ui;
using namespace se::ui::editor;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

double Milliseconds(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void LayoutTenThousandNodes()
{
    UiDocument document;
    UiStubTextService text;
    UiLayoutEngine layout(document, &text);
    const UiElementHandle root = document.Root();
    UiStyle rootStyle;
    rootStyle.width = UiLength::Percent(100.0f);
    rootStyle.height = UiLength::Percent(100.0f);
    rootStyle.flexDirection = UiFlexDirection::Column;
    document.SetStyle(root, rootStyle);
    for (int index = 0; index < 10000; ++index)
    {
        const UiElementHandle node = document.CreateElement(UiElementKind::Panel);
        Check(document.SetParent(node, root), "perf parent");
        UiStyle style;
        style.height = UiLength::Px(1.0f);
        document.SetStyle(node, style);
    }
    const auto start = std::chrono::steady_clock::now();
    Check(layout.Layout(1280.0f, 720.0f), "perf first layout");
    const double firstMs = Milliseconds(start);
    Check(!layout.Layout(1280.0f, 720.0f), "perf stable frame must not lay out");
    const UiLayoutStats after = layout.Stats();
    Check(after.layoutPasses == 0, "perf stable layout passes");
    Check(document.ElementCount() > 10000, "perf node count");
    std::cout << "UiPerf layout10k first=" << firstMs << "ms stableLayout=0\n";
}

void VirtualizedHundredThousandRows()
{
    UiDocument document;
    const UiElementHandle content = document.CreateElement(UiElementKind::Panel);
    Check(document.SetParent(content, document.Root()), "perf list parent");
    UiListView list(document, content);
    list.SetRowHeight(20.0f);
    list.SetViewportHeight(400.0f);
    list.SetItemCount(100000);
    list.Sync();
    Check(static_cast<int>(list.Rows().size()) < 80, "perf list virtualization");
    list.SetScrollOffset(500000.0f);
    list.Sync();
    Check(list.Offset() > 0.0f && static_cast<int>(list.Rows().size()) < 80,
          "perf list scrolled virtualization");
    std::cout << "UiPerf list100k rows=" << list.Rows().size() << " offset=" << list.Offset() << "\n";
}

void EditorShellIdleAndConsole()
{
    EditorShell shell;
    Check(shell.Initialize(1280.0f, 760.0f), "perf shell initialize");
    shell.Frame();
    Check(!shell.Layout().Layout(1280.0f, 760.0f), "perf shell idle layout");
    for (int index = 0; index < 100000; ++index)
    {
        shell.AppendConsoleLine("line " + std::to_string(index));
    }
    const auto start = std::chrono::steady_clock::now();
    shell.Frame();
    const double frameMs = Milliseconds(start);
    Check(shell.ConsoleLines().size() == 100003, "perf console lines");
    Check(static_cast<int>(shell.ConsoleList().Rows().size()) < 80, "perf console virtualization");
    Check(!shell.Layout().Layout(1280.0f, 760.0f), "perf shell stable after frame");
    std::cout << "UiPerf editorConsole100k frame=" << frameMs << "ms rows="
              << shell.ConsoleList().Rows().size() << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        LayoutTenThousandNodes();
        VirtualizedHundredThousandRows();
        EditorShellIdleAndConsole();
        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"layout10k\":true,\"list100k\":true,"
                   "\"editorIdleLayout0\":true,\"console100k\":true}\n";
        }
        std::cout << "UiPerfTests passed (10k layout, 100k list, idle zero-layout, 100k console).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
