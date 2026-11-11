#pragma once
#include "StaticModel.h"
#include <span>
#include <string_view>

namespace se::asset_io
{
inline constexpr std::uint64_t MaxFileBytes = 256ull * 1024 * 1024;
std::string Utf8(const std::filesystem::path& path);
std::filesystem::path FromUtf8(std::string_view text);
bool Within(const std::filesystem::path& root, const std::filesystem::path& path);
bool Read(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes);
// 新资源可使用“不覆盖”提交，避免与并发创建的文件冲突。
bool WriteAtomic(const std::filesystem::path& path,std::span<const std::uint8_t> bytes,bool replaceExisting=true);
std::string NewId();
std::string Hash(std::span<const std::uint8_t> bytes);
bool ResolveReference(const std::filesystem::path& source, const std::filesystem::path& root,
    std::string uri, std::filesystem::path& absolute, std::string& relative);
bool ResolveRelative(const std::filesystem::path& source, const std::filesystem::path& root,
    const std::string& path, std::filesystem::path& absolute, std::string& relative);
void Diagnostic(std::vector<AssetDiagnostic>& diagnostics, const char* key,
    std::string detail = {}, bool warning = false);
}
