#include "ToolLayer/Editor/ExternalChangeMonitor.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

int main()
{
    const auto root = std::filesystem::path(".cache/external-change-test") / "file.scene.json";
    std::error_code error;
    std::filesystem::create_directories(root.parent_path(), error);
    std::filesystem::remove(root, error);
    { std::ofstream file(root, std::ios::binary); file << "one"; }

    se::ExternalChangeMonitor monitor;
    monitor.Reset(root);
    CHECK(!monitor.Poll(), "no change immediately after reset");
    { std::ofstream file(root, std::ios::binary); file << "two-two"; }
    const auto future = std::filesystem::last_write_time(root, error) + std::chrono::seconds(2);
    CHECK(!error, "read mtime");
    std::filesystem::last_write_time(root, future, error);
    CHECK(!error, "write future mtime");
    CHECK(monitor.Poll(), "size/mtime change detected");
    monitor.Consume();
    CHECK(!monitor.Poll(), "consume updates baseline");

    std::filesystem::remove(root, error);
    CHECK(!monitor.Poll(), "missing file is not a change");
    std::filesystem::remove_all(root.parent_path(), error);
    std::cout << "ExternalChangeMonitorTests passed (baseline, mtime/size change, consume, missing file).\n";
    return 0;
}
