#pragma once
#include <bit>
#include <cstdint>

namespace se
{
// IEEE binary16，round-to-nearest-even；有限资源的范围验证由调用方负责。
inline std::uint16_t FloatToHalf(float value) noexcept
{
    const auto bits=std::bit_cast<std::uint32_t>(value);const auto sign=static_cast<std::uint16_t>((bits>>16)&0x8000);
    const auto exponent=(bits>>23)&255,mantissa=bits&0x7fffff;
    if(exponent==255)return static_cast<std::uint16_t>(sign|0x7c00|(mantissa?0x200:0));
    const int power=static_cast<int>(exponent)-127;
    if(power>15)return static_cast<std::uint16_t>(sign|0x7c00);
    if(power<-25)return sign;
    const auto round=[](std::uint32_t input,unsigned shift){
        const auto result=input>>shift,remainder=input&((1u<<shift)-1),middle=1u<<(shift-1);
        return result+static_cast<std::uint32_t>(remainder>middle||(remainder==middle&&(result&1)));};
    if(power<-14)return static_cast<std::uint16_t>(sign|round(mantissa|0x800000,static_cast<unsigned>(-power-1)));
    return static_cast<std::uint16_t>(sign|((static_cast<unsigned>(power+15)<<10)+round(mantissa,13)));
}
inline float HalfToFloat(std::uint16_t value) noexcept
{
    const std::uint32_t sign=std::uint32_t(value&0x8000)<<16,exponent=(value>>10)&31;
    std::uint32_t mantissa=value&1023;
    if(exponent==31)return std::bit_cast<float>(sign|0x7f800000|(mantissa<<13));
    if(exponent)return std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
    if(!mantissa)return std::bit_cast<float>(sign);
    int power=-14;while(!(mantissa&1024)){mantissa<<=1;--power;}
    return std::bit_cast<float>(sign|(static_cast<std::uint32_t>(power+127)<<23)|((mantissa&1023)<<13));
}
}
