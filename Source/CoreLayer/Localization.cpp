#include "CoreLayer/Localization.h"

#include <fstream>
#include <iterator>
#include <utility>

namespace
{
    // 拒绝截断、过长编码、代理项及超出 Unicode 范围的 UTF-8 序列。
    bool IsValidUtf8(std::string_view text)
    {
        std::size_t index = 0;
        while (index < text.size())
        {
            const auto first = static_cast<unsigned char>(text[index++]);
            if (first < 0x80) continue;
            unsigned int value = 0;
            unsigned int minimum = 0;
            int remaining = 0;
            if (first >= 0xC2 && first <= 0xDF)
            {
                value = first & 0x1F; minimum = 0x80; remaining = 1;
            }
            else if (first >= 0xE0 && first <= 0xEF)
            {
                value = first & 0x0F; minimum = 0x800; remaining = 2;
            }
            else if (first >= 0xF0 && first <= 0xF4)
            {
                value = first & 0x07; minimum = 0x10000; remaining = 3;
            }
            else return false;

            while (remaining-- > 0)
            {
                if (index == text.size()) return false;
                const auto next = static_cast<unsigned char>(text[index++]);
                if ((next & 0xC0) != 0x80) return false;
                value = (value << 6) | (next & 0x3F);
            }
            if (value < minimum || value > 0x10FFFF ||
                (value >= 0xD800 && value <= 0xDFFF)) return false;
        }
        return true;
    }
}

bool Localization::Load(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "Cannot open language table";
        return false;
    }
    std::string contents((std::istreambuf_iterator<char>(file)), {});
    if (file.bad() || !IsValidUtf8(contents))
    {
        error = "Cannot read language table or invalid UTF-8";
        return false;
    }
    if (contents.starts_with("\xEF\xBB\xBF")) contents.erase(0, 3);

    std::unordered_map<std::string, std::string> loaded;
    std::size_t start = 0;
    std::size_t lineNumber = 0;
    while (start < contents.size())
    {
        ++lineNumber;
        const auto end = contents.find('\n', start);
        std::string_view line(contents.data() + start,
            (end == std::string::npos ? contents.size() : end) - start);
        start = end == std::string::npos ? contents.size() : end + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() || line.starts_with('#')) continue;

        const auto separator = line.find('=');
        const auto key = line.substr(0, separator);
        const bool validKey = !key.empty() && key.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == std::string_view::npos;
        if (separator == std::string_view::npos || !validKey ||
            !loaded.emplace(std::string(key), std::string(line.substr(separator + 1))).second)
        {
            error = "Invalid or duplicate entry at line " + std::to_string(lineNumber);
            return false;
        }
    }
    if (loaded.empty())
    {
        error = "Language table is empty";
        return false;
    }
    texts_ = std::move(loaded);
    return true;
}

std::string Localization::GetText(std::string_view key) const
{
    const auto found = texts_.find(std::string(key));
    return found == texts_.end() ? std::string(key) : found->second;
}
