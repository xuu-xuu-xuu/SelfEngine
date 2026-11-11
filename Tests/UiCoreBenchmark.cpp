#include "FunctionLayer/UI/Core/UiDocument.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
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

constexpr std::size_t kNodeCount = 10000;
constexpr double kTimeLimitMs = 5000.0;
} // namespace

int main(int argc, char** argv)
{
    try
    {
        UiDocument document;
        const UiElementHandle root = document.Root();
        const UiElementHandle container = document.CreateElement(UiElementKind::Panel);
        Check(document.SetParent(container, root), "attach container");
        document.ClearAllDirty();

        std::vector<UiElementHandle> nodes;
        nodes.reserve(kNodeCount);

        const auto createStart = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < kNodeCount; ++index)
        {
            const UiElementHandle node = document.CreateElement(UiElementKind::Label);
            Check(document.SetParent(node, container), "attach benchmark node");
            nodes.push_back(node);
        }
        const auto createEnd = std::chrono::steady_clock::now();
        const double createMs = std::chrono::duration<double, std::milli>(createEnd - createStart).count();

        Check(document.ElementCount() == kNodeCount + 2, "benchmark element count");
        Check(document.ChildCount(container) == kNodeCount, "benchmark child count");
        Check(document.Element(nodes.front()) != nullptr && document.Element(nodes.back()) != nullptr, "benchmark handles");
        Check(document.Element(nodes.front())->Id() != document.Element(nodes.back())->Id(), "stable ids are distinct");
        Check(document.Find(document.Element(nodes.back())->Id()) == document.Element(nodes.back()), "find by id");

        const auto destroyStart = std::chrono::steady_clock::now();
        Check(document.DestroyElement(container), "destroy benchmark tree");
        const auto destroyEnd = std::chrono::steady_clock::now();
        const double destroyMs = std::chrono::duration<double, std::milli>(destroyEnd - destroyStart).count();

        Check(document.ElementCount() == 1, "only root remains after benchmark destroy");
        Check(document.Element(nodes.front()) == nullptr, "old benchmark handle invalid");
        Check(document.Element(container) == nullptr, "container handle invalid");
        Check(createMs < kTimeLimitMs, "create benchmark within debug budget");
        Check(destroyMs < kTimeLimitMs, "destroy benchmark within debug budget");

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"nodes\":" << kNodeCount
                << ",\"createMs\":" << createMs
                << ",\"destroyMs\":" << destroyMs
                << ",\"capacity\":" << document.Capacity()
                << ",\"remaining\":" << document.ElementCount() << "}\n";
        }
        std::cout << "UiCoreBenchmark: " << kNodeCount << " nodes create " << createMs
                  << " ms, destroy " << destroyMs << " ms, capacity " << document.Capacity() << ".\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
