#include "ToolLayer/Editor/BuildRestartRequest.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

int main()
{
    const auto path = std::filesystem::path(".cache/build-restart-test") / "restart.json";
    std::error_code error;
    std::filesystem::remove_all(path.parent_path(), error);
    std::string message;
    CHECK(se::WriteBuildRestartRequest(path, message), "write restart request");
    CHECK(message.empty(), "no error");
    std::ifstream input(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(content.find("\"schemaVersion\":1") != std::string::npos, "schema");
    CHECK(content.find("\"action\":\"compile\"") != std::string::npos, "action");
    std::filesystem::remove_all(path.parent_path(), error);
    std::cout << "BuildRestartRequestTests passed (compile request schema/action).\n";
    return 0;
}
