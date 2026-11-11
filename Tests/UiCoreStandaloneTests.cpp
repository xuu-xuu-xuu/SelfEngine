#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiElement.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

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
        using namespace se::ui;

        static_assert(sizeof(UiElementHandle) == 8, "handle is two 32-bit fields");
        static_assert(!std::is_copy_constructible_v<UiDocument>, "document is unique-owner");
        static_assert(!std::is_copy_assignable_v<UiDocument>, "document is unique-owner");
        static_assert(std::is_nothrow_move_constructible_v<UiDocument>, "document move is noexcept");
        static_assert(std::is_nothrow_move_assignable_v<UiDocument>, "document move assign is noexcept");

        UiDocument document;
        Check(document.IsValid(), "document constructs an implementation");

        const UiElement empty;
        Check(!empty.IsValid(), "default element is invalid");

        const UiElementHandle handle{1, 1};
        Check(handle.IsValid(), "valid handle distinguishes index and generation");
        Check(!UiElementHandle{0, 5}.IsValid(), "zero index is invalid");
        Check(!UiElementHandle{5, 0}.IsValid(), "zero generation is invalid");
        Check(!UiElementHandle{}.IsValid(), "default handle is invalid");
        Check(handle == UiElementHandle{1, 1}, "handle equality compares index and generation");
        Check(handle != UiElementHandle{1, 2}, "handle inequality compares generation");

        const UiElement element(handle);
        Check(element.IsValid() && element.Handle() == handle, "element carries a borrowed handle");

        UiDocument moved(std::move(document));
        Check(moved.IsValid(), "moved-to document is valid");
        Check(!document.IsValid(), "moved-from document is invalid");

        document = std::move(moved);
        Check(document.IsValid(), "move assignment restores document");
        Check(!moved.IsValid(), "move-assigned-from document is invalid");

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"standaloneLink\":true,\"pimplLifecycle\":true,"
                   "\"handleGeneration\":true,\"elements\":1}\n";
        }
        std::cout << "UiCoreStandaloneTests passed (core links only EngineCore; pimpl and handle generation).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
