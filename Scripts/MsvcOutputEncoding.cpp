#include <Windows.h>
#include <cstdio>

// MSVC 使用输出代码页，CMake AUTO 可能使用输入代码页；两者可以不同。
int main()
{
    const auto output = GetConsoleOutputCP();
    const auto codepage = output ? output : GetACP();
    const char* encoding = codepage == CP_UTF8 ? "UTF-8"
        : codepage == GetACP() ? "ANSI"
        : codepage == GetOEMCP() ? "OEM" : nullptr;
    if (!encoding) { std::printf("Unsupported compiler output code page: %u\n", codepage); return 1; }
    std::puts(encoding);
    return 0;
}
