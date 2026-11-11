#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace se::ui;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

double Milliseconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

UiElementHandle Add(UiDocument& document, UiElementHandle parent, const UiStyle& style)
{
    const UiElementHandle element = document.CreateElement(UiElementKind::Panel);
    Check(document.SetParent(element, parent), "attach benchmark node");
    Check(document.SetStyle(element, style), "set benchmark style");
    return element;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        constexpr std::size_t kChainDepth = 128;
        constexpr std::size_t kLeafCount = 9872;
        constexpr std::size_t kExpectedNodes = 1 + kChainDepth + kLeafCount;

        UiDocument document;
        UiStubTextService text(8.0f, 16.0f);
        UiLayoutEngine layout(document, &text);
        const UiElementHandle root = document.Root();
        UiStyle rootStyle;
        rootStyle.flexDirection = UiFlexDirection::Column;
        Check(document.SetStyle(root, rootStyle), "root style");

        UiElementHandle parent = root;
        UiElementHandle deepest = root;
        for (std::size_t index = 0; index < kChainDepth; ++index)
        {
            UiStyle chainStyle;
            chainStyle.width = UiLength::Percent(100.0f);
            chainStyle.flexDirection = UiFlexDirection::Column;
            chainStyle.flexShrink = 0.0f;
            deepest = Add(document, parent, chainStyle);
            parent = deepest;
        }
        UiStyle rowStyle = document.Style(deepest);
        rowStyle.flexDirection = UiFlexDirection::Row;
        Check(document.SetStyle(deepest, rowStyle), "deepest row style");

        std::vector<UiElementHandle> leaves;
        leaves.reserve(kLeafCount);
        UiStyle leafStyle;
        leafStyle.width = UiLength::Px(10.0f);
        leafStyle.height = UiLength::Px(10.0f);
        leafStyle.flexShrink = 0.0f;
        for (std::size_t index = 0; index < kLeafCount; ++index)
        {
            leaves.push_back(Add(document, deepest, leafStyle));
        }
        Check(document.ElementCount() == kExpectedNodes, "benchmark node count");

        const std::vector<float> widths{640.0f, 1280.0f, 1920.0f};
        std::ofstream report;
        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            report.open(proof);
            report << "{\"success\":true,\"nodes\":" << kExpectedNodes
                   << ",\"depth\":" << kChainDepth << ",\"runs\":[";
        }

        bool firstRun = true;
        int runIndex = 0;
        for (const float width : widths)
        {
            const auto firstStart = std::chrono::steady_clock::now();
            Check(layout.Layout(width, 720.0f), "first layout");
            const auto firstEnd = std::chrono::steady_clock::now();
            const UiLayoutStats firstStats = layout.Stats();
            const double firstMs = Milliseconds(firstStart, firstEnd);
            Check(firstStats.layoutPasses == 1, "first layout pass");
            Check(firstStats.nodesMeasured == kExpectedNodes, "first layout measures all nodes");

            const auto stableStart = std::chrono::steady_clock::now();
            Check(!layout.Layout(width, 720.0f), "stable frame skips layout");
            const auto stableEnd = std::chrono::steady_clock::now();
            const double stableMs = Milliseconds(stableStart, stableEnd);
            Check(layout.Stats().layoutPasses == 0, "stable frame zero passes");

            UiStyle changed = document.Style(leaves.back());
            changed.width = UiLength::Px(20.0f + static_cast<float>(runIndex));
            Check(document.SetStyle(leaves.back(), changed), "single node width");
            const auto changeStart = std::chrono::steady_clock::now();
            Check(layout.Layout(width, 720.0f), "layout after single change");
            const auto changeEnd = std::chrono::steady_clock::now();
            const UiLayoutStats changeStats = layout.Stats();
            const double changeMs = Milliseconds(changeStart, changeEnd);
            Check(changeStats.nodesMeasured < kExpectedNodes, "single change measures only dirty path");
            Check(firstMs < 2000.0, "first layout debug budget");
            Check(stableMs < 5.0, "stable frame debug budget");
            Check(changeMs < 2000.0, "single change debug budget");

            std::cout << "UiCoreLayoutBenchmark width=" << width
                      << " first=" << firstMs << " ms"
                      << " stable=" << stableMs << " ms"
                      << " change=" << changeMs << " ms"
                      << " measured=" << changeStats.nodesMeasured << '\n';

            ++runIndex;
            if (report.is_open())
            {
                if (!firstRun)
                {
                    report << ',';
                }
                firstRun = false;
                report << "{\"width\":" << width
                       << ",\"firstMs\":" << firstMs
                       << ",\"stableMs\":" << stableMs
                       << ",\"changeMs\":" << changeMs
                       << ",\"firstMeasured\":" << firstStats.nodesMeasured
                       << ",\"changeMeasured\":" << changeStats.nodesMeasured << '}';
            }
        }

        if (report.is_open())
        {
            report << "]}\n";
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
