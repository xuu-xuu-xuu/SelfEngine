#pragma once
#include <string>
#include <string_view>

namespace se
{
// 严格转换：拒绝过长编码、代理码点及超过 U+10FFFF 的输入；失败不修改输出。
bool DecodeUtf8(std::string_view text, std::u32string& output);
bool EncodeUtf8(std::u32string_view text, std::string& output);
bool DecodeUtf16(std::u16string_view text, std::u32string& output);
std::u16string EncodeUtf16(std::u32string_view text);
}
