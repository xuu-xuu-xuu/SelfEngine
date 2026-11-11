#include "Utf.h"

namespace se
{
namespace { bool Scalar(char32_t c) { return c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff); } }
bool DecodeUtf8(std::string_view text, std::u32string& output)
{
    std::u32string result;
    for (std::size_t i = 0; i < text.size();)
    {
        const auto first = static_cast<unsigned char>(text[i++]);
        char32_t value = first;
        unsigned count = 0;
        char32_t minimum = 0;
        if (first < 0x80) {}
        else if (first >= 0xc2 && first <= 0xdf) { value = first & 0x1f; count = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 0x0f; count = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 0x07; count = 3; minimum = 0x10000; }
        else return false;
        if (i + count > text.size()) return false;
        for (unsigned n = 0; n < count; ++n)
        {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || !Scalar(value)) return false;
        result.push_back(value);
    }
    output = std::move(result);
    return true;
}
bool EncodeUtf8(std::u32string_view text, std::string& output)
{
    std::string result;
    for (char32_t value : text)
    {
        if (!Scalar(value)) return false;
        if (value < 0x80) result.push_back(static_cast<char>(value));
        else if (value < 0x800)
        {
            result.push_back(static_cast<char>(0xc0 | (value >> 6)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
        else if (value < 0x10000)
        {
            result.push_back(static_cast<char>(0xe0 | (value >> 12)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
        else
        {
            result.push_back(static_cast<char>(0xf0 | (value >> 18)));
            result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
    }
    output = std::move(result);
    return true;
}
bool DecodeUtf16(std::u16string_view text, std::u32string& output)
{
    std::u32string result;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        char32_t c = text[i];
        if (c >= 0xd800 && c <= 0xdbff)
        {
            if (++i >= text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) return false;
            c = 0x10000 + ((c - 0xd800) << 10) + (text[i] - 0xdc00);
        }
        else if (c >= 0xdc00 && c <= 0xdfff) return false;
        result.push_back(c);
    }
    output = std::move(result);
    return true;
}
std::u16string EncodeUtf16(std::u32string_view text)
{
    std::u16string result;
    for (char32_t c : text)
    {
        if (!Scalar(c)) c = 0xfffd;
        if (c < 0x10000) result.push_back(static_cast<char16_t>(c));
        else { c -= 0x10000; result.push_back(static_cast<char16_t>(0xd800 + (c >> 10))); result.push_back(static_cast<char16_t>(0xdc00 + (c & 0x3ff))); }
    }
    return result;
}
}
