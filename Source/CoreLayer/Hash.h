#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
namespace se
{
// 原始字节 SHA-256，小写十六进制；平台失败或单次输入过大时返回空串。
std::string Sha256(std::span<const std::uint8_t> bytes);
inline std::string Sha256(std::string_view text)
{return Sha256(std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));}
}
