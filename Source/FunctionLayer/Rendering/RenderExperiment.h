#pragma once
#include <cstdint>
#include <string_view>

namespace se
{
// G00：G 系列实验开关。默认 Off；同一场景只允许一个实验开启，
// 关闭时必须回到 R 系列验收过的画面。
enum class RenderExperiment : std::uint8_t
{
    Off = 0,
    G01 = 1,
    G02 = 2,
    G03 = 3,
    G04 = 4,
    G05 = 5,
    G06 = 6,
    G07 = 7,
    G08 = 8,
    G09 = 9,
    G10 = 10,
    G11 = 11,
    G12 = 12,
    G13 = 13,
    G14 = 14,
    G15 = 15
};

inline constexpr std::string_view RenderExperimentName(RenderExperiment value) noexcept
{
    switch (value)
    {
    case RenderExperiment::Off: return "off";
    case RenderExperiment::G01: return "g01";
    case RenderExperiment::G02: return "g02";
    case RenderExperiment::G03: return "g03";
    case RenderExperiment::G04: return "g04";
    case RenderExperiment::G05: return "g05";
    case RenderExperiment::G06: return "g06";
    case RenderExperiment::G07: return "g07";
    case RenderExperiment::G08: return "g08";
    case RenderExperiment::G09: return "g09";
    case RenderExperiment::G10: return "g10";
    case RenderExperiment::G11: return "g11";
    case RenderExperiment::G12: return "g12";
    case RenderExperiment::G13: return "g13";
    case RenderExperiment::G14: return "g14";
    case RenderExperiment::G15: return "g15";
    default: return "off";
    }
}

// 只接受 off 或 g01..g15；未知值返回 false，由调用者拒绝启动。
inline constexpr bool ParseRenderExperiment(std::string_view text, RenderExperiment& output) noexcept
{
    if (text == "off")
    {
        output = RenderExperiment::Off;
        return true;
    }
    if (text.size() != 3 || text[0] != 'g' || text[1] < '0' || text[1] > '1' || text[2] < '0' || text[2] > '9')
        return false;
    const int number = (text[1] - '0') * 10 + (text[2] - '0');
    if (number < 1 || number > 15)
        return false;
    output = static_cast<RenderExperiment>(number);
    return true;
}
}
