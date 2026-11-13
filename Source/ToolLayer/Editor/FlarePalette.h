#pragma once

#include <cstdint>

namespace se::flare
{
// sRGB 界面色，含义见《引擎自带UI风格》。在绘制边界转换为各后端需要的表示。
struct Color { std::uint8_t r, g, b; };
inline constexpr Color Background{22,24,28};
inline constexpr Color Surface{32,35,41};
inline constexpr Color Raised{43,47,54};
inline constexpr Color Border{53,58,68};
inline constexpr Color Hover{58,64,74};
inline constexpr Color Selected{73,81,93};
inline constexpr Color Heading{237,240,245};
inline constexpr Color Text{211,215,224};
inline constexpr Color Muted{157,166,182};
inline constexpr Color Disabled{118,127,142};
inline constexpr Color Silver{194,202,215};
inline constexpr Color Gold{254,206,123}; // B 款图标亮部的中位色。

template<class T>
constexpr T Rgba(Color color, float alpha = 1.0f)
{
    return {color.r/255.0f, color.g/255.0f, color.b/255.0f, alpha};
}
}
