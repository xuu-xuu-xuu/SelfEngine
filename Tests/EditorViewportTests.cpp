#include "ToolLayer/Editor/EditorUi/EditorViewportMath.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace se::ui::editor;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void ViewportMath()
{
    const ViewportRectF rect{100.0f, 50.0f, 400.0f, 200.0f};
    Check(ViewportContains(rect, 100.0f, 50.0f), "top-left inside");
    Check(ViewportContains(rect, 499.0f, 249.0f), "bottom-right inside");
    Check(!ViewportContains(rect, 500.0f, 250.0f), "exclusive bottom-right");
    Check(!ViewportContains(rect, 99.0f, 60.0f), "outside left");
    Check(!ViewportContains(ViewportRectF{}, 0.0f, 0.0f), "empty viewport");

    float u = 0.0f;
    float v = 0.0f;
    ViewportNormalize(rect, 300.0f, 150.0f, u, v);
    Check(std::abs(u - 0.5f) < 1e-5f && std::abs(v - 0.5f) < 1e-5f, "normalize center");
    ViewportNormalize(rect, 100.0f, 50.0f, u, v);
    Check(u == 0.0f && v == 0.0f, "normalize origin");

    float x = 300.0f;
    float y = 250.0f;
    ViewportOffsetPlayInput(rect, 2.0f, x, y);
    Check(x == 100.0f && y == 150.0f, "dpi 2 offset");
    float fallbackX = 300.0f;
    float fallbackY = 250.0f;
    ViewportOffsetPlayInput(rect, 0.0f, fallbackX, fallbackY);
    Check(fallbackX == 200.0f && fallbackY == 200.0f, "dpi fallback");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        ViewportMath();
        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof) << "{\"success\":true,\"normalize\":true,\"dpiOffset\":true}\n";
        }
        std::cout << "EditorViewportTests passed (contains/normalize/DPI offset).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
