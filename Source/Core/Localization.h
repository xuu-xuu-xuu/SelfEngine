#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

// 独立的 UTF-8 文本表；不负责选择语言、输出日志或定位资源目录。
class Localization
{
public:
    // 文件格式为 key=value。加载失败保留原表，并通过 error 返回原因。
    bool Load(const std::filesystem::path& path, std::string& error);
    // 返回副本，避免后续换表使调用者持有的文字失效；缺失时返回键名。
    std::string GetText(std::string_view key) const;

private:
    std::unordered_map<std::string, std::string> texts_;
};
