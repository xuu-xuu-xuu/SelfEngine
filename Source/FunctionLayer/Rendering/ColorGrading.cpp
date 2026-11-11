#include "ColorGrading.h"
#include "CoreLayer/Math/Half.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace se
{
namespace
{
float Clamp01(float value){return std::clamp(value,0.0f,1.0f);}
float EncodeSrgb(float value)
{
    const auto clamped=Clamp01(value);
    return clamped<=0.0031308f?12.92f*clamped:1.055f*std::pow(clamped,1.0f/2.4f)-0.055f;
}
float DecodeSrgb(float value)
{
    const auto clamped=Clamp01(value);
    return clamped<=0.04045f?clamped/12.92f:std::pow((clamped+0.055f)/1.055f,2.4f);
}
void AppendUint32(std::vector<std::uint8_t>& bytes,std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value>>8));
    bytes.push_back(static_cast<std::uint8_t>(value>>16));
    bytes.push_back(static_cast<std::uint8_t>(value>>24));
}
void AppendFloat(std::vector<std::uint8_t>& bytes,float value)
{
    std::uint32_t bits=0;std::memcpy(&bits,&value,sizeof(bits));AppendUint32(bytes,bits);
}
std::uint32_t ReadUint32(const std::uint8_t*& cursor)
{
    const auto value=std::uint32_t(cursor[0])|(std::uint32_t(cursor[1])<<8)
        |(std::uint32_t(cursor[2])<<16)|(std::uint32_t(cursor[3])<<24);
    cursor+=4;return value;
}
float ReadFloat(const std::uint8_t*& cursor)
{
    const auto bits=ReadUint32(cursor);float value=0;std::memcpy(&value,&bits,sizeof(value));return value;
}
bool FiniteLutValue(Vec3 value)
{
    // 线性 LUT 允许 >1 的显示值（例如 gain>1，最终编码时再饱和）；上限与 RGBA16F 一致。
    return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)
        &&value.x>=0&&value.x<=65504&&value.y>=0&&value.y<=65504&&value.z>=0&&value.z<=65504;
}
bool SrgbEncodable(Vec3 value)
{
    return value.x<=1&&value.y<=1&&value.z<=1;
}
bool ReadFile(const std::filesystem::path& path,std::vector<std::uint8_t>& bytes)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)return false;
    const auto size=file.tellg();
    if(size<0||static_cast<std::uint64_t>(size)>64ull*1024*1024)return false;
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0,std::ios::beg);
    if(!bytes.empty())file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    return bool(file);
}
bool WriteFile(const std::filesystem::path& path,const std::vector<std::uint8_t>& bytes)
{
    std::error_code ignored;
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),ignored);
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    if(!file)return false;
    if(!bytes.empty())file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    return bool(file);
}
Vec3 MulRows(const float matrix[3][3],Vec3 color)
{
    return {matrix[0][0]*color.x+matrix[0][1]*color.y+matrix[0][2]*color.z,
        matrix[1][0]*color.x+matrix[1][1]*color.y+matrix[1][2]*color.z,
        matrix[2][0]*color.x+matrix[2][1]*color.y+matrix[2][2]*color.z};
}
float AcesFit(float value)
{
    const auto numerator=value*(value+0.0245786f)-0.000090537f;
    const auto denominator=value*(0.983729f*value+0.4329510f)+0.238081f;
    return std::abs(denominator)>1e-12f?numerator/denominator:0.0f;
}
}
bool ColorLutAsset::Valid() const noexcept
{
    if(size<2||size>65||values.size()!=std::size_t{size}*size*size)return false;
    return std::all_of(values.begin(),values.end(),FiniteLutValue);
}
bool WriteColorLutAsset(const std::filesystem::path& path,const ColorLutAsset& asset,std::string& error)
{
    if(!asset.Valid()){error="Rendering.LutAssetInvalid";return false;}
    if(asset.colorSpace!=ColorLutColorSpace::Linear&&asset.colorSpace!=ColorLutColorSpace::Srgb)
    {error="Rendering.LutAssetInvalid";return false;}
    // sRGB 标记要求值域可编码；>1 的线性 LUT 只能用 Linear 标记保存。
    if(asset.colorSpace==ColorLutColorSpace::Srgb
        &&!std::all_of(asset.values.begin(),asset.values.end(),SrgbEncodable))
    {error="Rendering.LutAssetInvalid";return false;}
    std::vector<std::uint8_t> bytes;
    bytes.reserve(24+std::size_t{3}*4*asset.values.size());
    const char magic[4]={'S','E','L','T'};bytes.insert(bytes.end(),magic,magic+4);
    AppendUint32(bytes,ColorLutAssetVersion);
    AppendUint32(bytes,asset.size);
    AppendUint32(bytes,3);
    AppendUint32(bytes,static_cast<std::uint32_t>(asset.colorSpace));
    AppendUint32(bytes,static_cast<std::uint32_t>(asset.values.size()));
    for(const auto& value:asset.values)
    {
        const auto encode=[&](float channel){return asset.colorSpace==ColorLutColorSpace::Srgb?EncodeSrgb(channel):channel;};
        AppendFloat(bytes,encode(value.x));AppendFloat(bytes,encode(value.y));AppendFloat(bytes,encode(value.z));
    }
    if(!WriteFile(path,bytes)){error="Assets.WriteFailed";return false;}
    return true;
}
bool ReadColorLutAsset(const std::filesystem::path& path,ColorLutAsset& output,std::string& error)
{
    std::vector<std::uint8_t> bytes;
    if(!ReadFile(path,bytes)){error="Assets.ReadFailed";return false;}
    if(bytes.size()<24||std::memcmp(bytes.data(),"SELT",4)!=0){error="Rendering.LutAssetInvalid";return false;}
    const auto* cursor=bytes.data()+4;const auto* end=bytes.data()+bytes.size();
    const auto version=ReadUint32(cursor);
    const auto size=ReadUint32(cursor);
    const auto channels=ReadUint32(cursor);
    const auto colorSpaceValue=ReadUint32(cursor);
    const auto count=ReadUint32(cursor);
    if(version!=ColorLutAssetVersion||size<2||size>65||channels!=3
        ||colorSpaceValue>static_cast<std::uint32_t>(ColorLutColorSpace::Srgb)
        ||count!=std::uint32_t{size}*size*size){error="Rendering.LutAssetInvalid";return false;}
    if(static_cast<std::uint64_t>(cursor-bytes.data())+std::uint64_t{count}*12!=bytes.size())
    {error="Rendering.LutAssetInvalid";return false;}
    ColorLutAsset candidate;
    candidate.size=size;candidate.colorSpace=static_cast<ColorLutColorSpace>(colorSpaceValue);
    candidate.values.resize(count);
    for(auto& value:candidate.values)
    {
        value={ReadFloat(cursor),ReadFloat(cursor),ReadFloat(cursor)};
        if(!FiniteLutValue(value)||(candidate.colorSpace==ColorLutColorSpace::Srgb&&!SrgbEncodable(value)))
        {error="Rendering.LutAssetInvalid";return false;}
    }
    if(cursor!=end){error="Rendering.LutAssetInvalid";return false;}
    output=std::move(candidate);return true;
}
Vec3 TonemapReinhard(Vec3 linear)
{
    const auto x=std::max(0.0f,linear.x),y=std::max(0.0f,linear.y),z=std::max(0.0f,linear.z);
    return {x/(1.0f+x),y/(1.0f+y),z/(1.0f+z)};
}
Vec3 TonemapAcesFitted(Vec3 linear)
{
    static constexpr float input[3][3]={{0.59719f,0.35458f,0.04823f},{0.07600f,0.90834f,0.01566f},{0.02840f,0.13383f,0.83777f}};
    static constexpr float output[3][3]={{1.60475f,-0.53108f,-0.07367f},{-0.10208f,1.10813f,-0.00605f},{-0.00327f,-0.07276f,1.07602f}};
    const auto fitted=MulRows(input,{std::max(0.0f,linear.x),std::max(0.0f,linear.y),std::max(0.0f,linear.z)});
    const auto mapped=Vec3{AcesFit(fitted.x),AcesFit(fitted.y),AcesFit(fitted.z)};
    const auto graded=MulRows(output,mapped);
    return {Clamp01(graded.x),Clamp01(graded.y),Clamp01(graded.z)};
}
Vec3 Tonemap(Vec3 linear,ToneMapper mapper)
{
    return mapper==ToneMapper::AcesFitted?TonemapAcesFitted(linear):TonemapReinhard(linear);
}
Vec3 SrgbToLinear(Vec3 encoded)
{
    return {DecodeSrgb(encoded.x),DecodeSrgb(encoded.y),DecodeSrgb(encoded.z)};
}
Vec3 LinearToSrgb(Vec3 linear)
{
    return {EncodeSrgb(linear.x),EncodeSrgb(linear.y),EncodeSrgb(linear.z)};
}
Vec3 ApplyLiftGammaGain(Vec3 linear,const ColorGradingSettings& settings)
{
    const auto apply=[&](float value,float lift,float gamma,float gain)
    {
        const auto safeGamma=std::isfinite(gamma)&&gamma>1e-4f?gamma:1.0f;
        const auto offset=std::isfinite(lift)?lift:0.0f;
        const auto scale=std::isfinite(gain)?gain:1.0f;
        const auto graded=std::max(0.0f,value*scale+offset);
        return std::pow(graded,1.0f/safeGamma);
    };
    return {apply(linear.x,settings.lift[0],settings.gamma[0],settings.gain[0]),
        apply(linear.y,settings.lift[1],settings.gamma[1],settings.gain[1]),
        apply(linear.z,settings.lift[2],settings.gamma[2],settings.gain[2])};
}
bool ColorLut::Build(std::uint32_t size,const ColorGradingSettings& settings)
{
    if(size<2||size>65)return false;
    std::vector<Vec3> values(std::size_t(size)*size*size);
    for(std::uint32_t red=0;red<size;++red)
        for(std::uint32_t green=0;green<size;++green)
            for(std::uint32_t blue=0;blue<size;++blue)
            {
                const auto r=static_cast<float>(red)/static_cast<float>(size-1);
                const auto g=static_cast<float>(green)/static_cast<float>(size-1);
                const auto b=static_cast<float>(blue)/static_cast<float>(size-1);
                values[(std::size_t(red)*size+green)*size+blue]=ApplyLiftGammaGain({r,g,b},settings);
            }
    size_=size;
    values_=std::move(values);
    assetColorSpace_=ColorLutColorSpace::Linear;
    return true;
}
void ColorLut::Clear() noexcept
{
    size_=0;
    assetColorSpace_=ColorLutColorSpace::Linear;
    values_.clear();
}
bool ColorLut::SaveAsset(const std::filesystem::path& path,ColorLutColorSpace space,std::string& error) const
{
    if(!Ready()){error="Rendering.LutAssetInvalid";return false;}
    ColorLutAsset asset;asset.size=size_;asset.colorSpace=space;asset.values=values_;
    return WriteColorLutAsset(path,asset,error);
}
bool ColorLut::Adopt(std::uint32_t size,std::vector<Vec3> values,ColorLutColorSpace sourceSpace)
{
    if(size<2||size>65||values.size()!=std::size_t{size}*size*size
        ||sourceSpace!=ColorLutColorSpace::Linear&&sourceSpace!=ColorLutColorSpace::Srgb)return false;
    for(const auto& value:values)if(!FiniteLutValue(value))return false;
    size_=size;
    values_=std::move(values);
    assetColorSpace_=sourceSpace;
    return true;
}
bool ColorLut::LoadAsset(const std::filesystem::path& path,std::string& error)
{
    ColorLutAsset asset;
    if(!ReadColorLutAsset(path,asset,error))return false;
    if(asset.colorSpace==ColorLutColorSpace::Srgb)
        for(auto& value:asset.values)value=SrgbToLinear(value);
    return Adopt(asset.size,std::move(asset.values),asset.colorSpace);
}
Vec3 ColorLut::Sample(Vec3 linear) const
{
    if(!Ready())return linear;
    const auto scale=static_cast<float>(size_-1);
    const auto x=std::clamp(linear.x,0.0f,1.0f)*scale;
    const auto y=std::clamp(linear.y,0.0f,1.0f)*scale;
    const auto z=std::clamp(linear.z,0.0f,1.0f)*scale;
    const auto x0=static_cast<std::uint32_t>(x),y0=static_cast<std::uint32_t>(y),z0=static_cast<std::uint32_t>(z);
    const auto x1=std::min(x0+1,size_-1),y1=std::min(y0+1,size_-1),z1=std::min(z0+1,size_-1);
    const auto tx=x-static_cast<float>(x0),ty=y-static_cast<float>(y0),tz=z-static_cast<float>(z0);
    const auto fetch=[&](std::uint32_t red,std::uint32_t green,std::uint32_t blue)
    {return values_[(std::size_t(red)*size_+green)*size_+blue];};
    const auto interpolate=[](Vec3 a,Vec3 b,float weight){return a*(1.0f-weight)+b*weight;};
    const auto c00=interpolate(fetch(x0,y0,z0),fetch(x1,y0,z0),tx);
    const auto c10=interpolate(fetch(x0,y1,z0),fetch(x1,y1,z0),tx);
    const auto c01=interpolate(fetch(x0,y0,z1),fetch(x1,y0,z1),tx);
    const auto c11=interpolate(fetch(x0,y1,z1),fetch(x1,y1,z1),tx);
    return interpolate(interpolate(c00,c10,ty),interpolate(c01,c11,ty),tz);
}
Vec3 ApplyGrading(Vec3 displayLinear,const ColorGradingSettings& settings,const ColorLut* lut)
{
    if(settings.lutEnabled&&lut&&lut->Ready())
        return lut->Sample({Clamp01(displayLinear.x),Clamp01(displayLinear.y),Clamp01(displayLinear.z)});
    return ApplyLiftGammaGain(displayLinear,settings);
}
Vec3 GradeColor(Vec3 hdrLinear,const ColorGradingSettings& settings,const ColorLut* lut)
{
    return ApplyGrading(Tonemap(hdrLinear,settings.toneMapper),settings,lut);
}
Vec3 PresentSrgb(Vec3 hdrLinear,const ColorGradingSettings& settings,const ColorLut* lut)
{
    return LinearToSrgb(GradeColor(hdrLinear,settings,lut));
}
Vec3 ComposeUi(Vec3 hdrLinear,Vec3 uiSrgb,float alpha,float exposure,const ColorGradingSettings& settings,
    const ColorLut* lut)
{
    const auto safeExposure=std::isfinite(exposure)&&exposure>0.0f?exposure:1.0f;
    // UI 与 R 系列一致：在 tonemap/调色后的显示线性空间混合，最后统一编码一次。
    const auto background=GradeColor(hdrLinear*safeExposure,settings,lut);
    const auto uiLinear=SrgbToLinear(uiSrgb);
    const auto weight=Clamp01(std::isfinite(alpha)?alpha:1.0f);
    return LinearToSrgb(background*(1.0f-weight)+uiLinear*weight);
}
bool ColorLutStrip::Valid() const noexcept
{
    const auto expectedPerPixel=format==ColorLutStripFormat::Rgba8?4u:format==ColorLutStripFormat::Rgba16Float?8u:0u;
    return expectedPerPixel&&bytesPerPixel==expectedPerPixel&&height>1&&width==height*height
        &&bytes.size()==std::size_t(width)*height*bytesPerPixel;
}
bool BakeColorLutStrip(const ColorLut& lut,ColorLutStripFormat format,ColorLutStrip& output)
{
    if(!lut.Ready()||(format!=ColorLutStripFormat::Rgba8&&format!=ColorLutStripFormat::Rgba16Float))return false;
    const auto size=lut.Size();
    ColorLutStrip strip;
    strip.width=size*size;strip.height=size;strip.format=format;
    strip.bytesPerPixel=format==ColorLutStripFormat::Rgba8?4u:8u;
    strip.bytes.assign(std::size_t(strip.width)*strip.height*strip.bytesPerPixel,0);
    const auto& values=lut.Values();
    constexpr std::uint16_t opaqueHalf=0x3c00; // 1.0h
    // strip 布局：x = blue*size + red，y = green；与 FinalOutput.hlsl 的 LutTexel 一致。
    for(std::uint32_t blue=0;blue<size;++blue)
        for(std::uint32_t green=0;green<size;++green)
            for(std::uint32_t red=0;red<size;++red)
            {
                const auto& value=values[(std::size_t{red}*size+green)*size+blue];
                const float channels[3]={value.x,value.y,value.z};
                auto* pixel=strip.bytes.data()+(std::size_t{green}*strip.width+blue*size+red)*strip.bytesPerPixel;
                for(std::uint32_t channel=0;channel<3;++channel)
                {
                    const auto input=std::isfinite(channels[channel])?channels[channel]:0.0f;
                    if(format==ColorLutStripFormat::Rgba8)
                        pixel[channel]=static_cast<std::uint8_t>(std::lround(std::clamp(input,0.0f,1.0f)*255.0f));
                    else
                    {
                        const auto half=FloatToHalf(input);
                        pixel[channel*2]=static_cast<std::uint8_t>(half);
                        pixel[channel*2+1]=static_cast<std::uint8_t>(half>>8);
                    }
                }
                if(format==ColorLutStripFormat::Rgba8)pixel[3]=255;
                else
                {
                    pixel[6]=static_cast<std::uint8_t>(opaqueHalf);
                    pixel[7]=static_cast<std::uint8_t>(opaqueHalf>>8);
                }
            }
    output=std::move(strip);return true;
}
}
