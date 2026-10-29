#include "SandboxApp.h"
#include "T02Examples.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <Windows.h>

int main(int argc, char* argv[])
{
    std::string locale = "zh-CN";
    if (argc == 3 && std::string(argv[1]) == "--language" &&
        (std::string(argv[2]) == "zh-CN" || std::string(argv[2]) == "en-US"))
    {
        locale = argv[2];
    }
    else if (argc != 1)
    {
        std::cerr << "Usage: EngineSandbox [--language zh-CN|en-US]\n";
        return 2;
    }
    static_assert(sizeof(void*) == 8, "自研引擎需要64位编译器");
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "当前工作目录：" << std::filesystem::current_path() << '\n';

    const int exampleResult = RunT02Examples();
    if (exampleResult != 0)
    {
        return exampleResult;
    }
    return RunSandboxApp(locale);
}
