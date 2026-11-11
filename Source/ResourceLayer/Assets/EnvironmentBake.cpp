#include "Environment.h"
#include "AssetIO.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Math/Half.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>

namespace se
{
namespace
{
constexpr double pi=3.14159265358979323846;
bool Digest(std::string_view hash){return hash.size()==64&&std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool Power(std::uint32_t size,std::uint32_t maximum){return size>=2&&size<=maximum&&(size&(size-1))==0;}
std::uint32_t Levels(std::uint32_t size){return static_cast<std::uint32_t>(std::bit_width(size));}
float Radical(std::uint32_t bits)
{
    bits=(bits<<16)|(bits>>16);bits=((bits&0x55555555)<<1)|((bits&0xaaaaaaaa)>>1);
    bits=((bits&0x33333333)<<2)|((bits&0xcccccccc)>>2);bits=((bits&0x0f0f0f0f)<<4)|((bits&0xf0f0f0f0)>>4);
    bits=((bits&0x00ff00ff)<<8)|((bits&0xff00ff00)>>8);return static_cast<float>(double(bits)*2.3283064365386963e-10);
}
Vec3 Frame(Vec3 normal,Vec3 sample)
{
    const Vec3 up=std::abs(normal.z)<0.999f?Vec3{0,0,1}:Vec3{1,0,0};const auto tangent=Normalize(Cross(up,normal)),bitangent=Cross(normal,tangent);
    return Normalize(tangent*sample.x+bitangent*sample.y+normal*sample.z);
}
Vec3 Ggx(Vec2 sample,float roughness)
{
    const double alpha=double(std::max(0.045f,roughness))*std::max(0.045f,roughness),a2=alpha*alpha;
    const double cosine=std::sqrt((1-sample.x)/(1+(a2-1)*sample.x)),sine=std::sqrt(std::max(0.0,1-cosine*cosine)),phi=2*pi*sample.y;
    return {float(std::cos(phi)*sine),float(std::sin(phi)*sine),float(cosine)};
}
std::vector<Vec2> Samples(std::uint32_t count)
{std::vector<Vec2> result;result.reserve(count);for(std::uint32_t index=0;index<count;++index)result.push_back({float(index)/float(count),Radical(index)});return result;}
void Store(std::vector<std::uint16_t>& data,size_t pixel,Vec3 color,bool& clamped)
{
    const float values[]{color.x,color.y,color.z,1};
    for(size_t channel=0;channel<4;++channel)
    {
        const float value=std::clamp(values[channel],0.0f,65504.0f);clamped|=value!=values[channel];data[pixel*4+channel]=FloatToHalf(value);
    }
}
EnvironmentTexture Texture(std::uint32_t size,std::uint32_t levels,bool cube)
{
    EnvironmentTexture result{size,levels,cube,{}};result.subresources.resize(size_t(cube?6:1)*levels);
    for(std::uint32_t face=0;face<(cube?6u:1u);++face)for(std::uint32_t mip=0;mip<levels;++mip)
    {const auto width=std::max(1u,size>>mip);result.subresources[size_t(face)*levels+mip].resize(size_t(width)*width*4);}
    return result;
}
}
bool ValidateEnvironmentSettings(const EnvironmentSettings& settings)
{
    if(!Power(settings.cubeSize,256)||!Power(settings.irradianceSize,64)||!Power(settings.lutSize,256)||settings.samples<32||settings.samples>2048)return false;
    const auto cost=(std::uint64_t(settings.cubeSize)*settings.cubeSize*6+std::uint64_t(settings.irradianceSize)*settings.irradianceSize*6+std::uint64_t(settings.lutSize)*settings.lutSize)*settings.samples;
    return cost<=100'000'000;
}
std::string EnvironmentCookKey(std::string_view sourceHash,const EnvironmentSettings& settings)
{
    if(!Digest(sourceHash)||!ValidateEnvironmentSettings(settings))return {};
    const nlohmann::json key{{"algorithm",EnvironmentCookerVersion},{"source",sourceHash},{"cube",settings.cubeSize},{"irradiance",settings.irradianceSize},
        {"lut",settings.lutSize},{"samples",settings.samples},{"faces","+X-X+Y-Y+Z-Z"},{"transfer","linear-rgba16f"}};return Sha256(key.dump());
}
bool ValidateEnvironment(const EnvironmentAsset& asset)
{
    if(asset.cookKey.empty()||asset.cookKey!=EnvironmentCookKey(asset.sourceHash,asset.settings))return false;
    const std::uint32_t sizes[]{asset.settings.irradianceSize,asset.settings.cubeSize,asset.settings.lutSize};
    for(size_t index=0;index<3;++index)
    {
        const auto& texture=asset.textures[index];const auto levels=index==1?Levels(sizes[index]):1u;const bool cube=index<2;
        if(texture.size!=sizes[index]||texture.mipLevels!=levels||texture.cube!=cube||texture.subresources.size()!=size_t(cube?6:1)*levels)return false;
        for(size_t sub=0;sub<texture.subresources.size();++sub)
        {
            const auto width=std::max(1u,texture.size>>static_cast<unsigned>(sub%levels));const auto& pixels=texture.subresources[sub];if(pixels.size()!=size_t(width)*width*4)return false;
            for(const auto bits:pixels){const float value=HalfToFloat(bits);if(!IsFinite(value)||value<0||value>65504)return false;}
        }
    }
    return true;
}
Vec3 EnvironmentCubeDirection(std::uint32_t face,float u,float v)
{
    const float x=u*2-1,y=v*2-1;
    switch(face)
    {
    case 0:return Normalize(Vec3{1,-y,-x});case 1:return Normalize(Vec3{-1,-y,x});case 2:return Normalize(Vec3{x,1,y});
    case 3:return Normalize(Vec3{x,-1,-y});case 4:return Normalize(Vec3{x,-y,1});case 5:return Normalize(Vec3{-x,-y,-1});default:return {};
    }
}
Vec3 SampleEquirectangular(const FloatImage& image,Vec3 direction)
{
    if(!image.width||!image.height||image.pixels.size()!=size_t(image.width)*image.height||!IsFinite(direction)||!IsFinite(LengthSquared(direction))||LengthSquared(direction)<1e-12f)return {};
    direction=Normalize(direction);const double u=std::atan2(direction.z,direction.x)/(2*pi)+0.5,v=std::acos(std::clamp(double(direction.y),-1.0,1.0))/pi;
    const double px=u*image.width-0.5,py=std::clamp(v*image.height-0.5,0.0,double(image.height-1));const auto left=static_cast<int>(std::floor(px));const auto top=static_cast<std::uint32_t>(std::floor(py));
    const auto wrapped=[&](int x){return static_cast<std::uint32_t>((x%int(image.width)+int(image.width))%int(image.width));};
    const auto at=[&](std::uint32_t x,std::uint32_t y){return image.pixels[size_t(y)*image.width+x];};
    const float fx=static_cast<float>(px-std::floor(px)),fy=static_cast<float>(py-top);const auto right=wrapped(left+1),bottom=std::min(top+1,image.height-1);
    return (at(wrapped(left),top)*(1-fx)+at(right,top)*fx)*(1-fy)+(at(wrapped(left),bottom)*(1-fx)+at(right,bottom)*fx)*fy;
}
bool BakeEnvironment(const FloatImage& image,std::string sourceHash,const EnvironmentSettings& settings,EnvironmentAsset& output,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto key=EnvironmentCookKey(sourceHash,settings);
    if(key.empty()||!image.width||!image.height||image.width>16384||image.height>16384||std::uint64_t(image.width)*image.height>16*1024*1024||image.pixels.size()!=size_t(image.width)*image.height)
    {asset_io::Diagnostic(diagnostics,"Environment.CookInvalid");return false;}
    FloatImage input=image;bool clamped=false;
    for(auto& pixel:input.pixels)
    {
        if(!IsFinite(pixel)||pixel.x<0||pixel.y<0||pixel.z<0){asset_io::Diagnostic(diagnostics,"Environment.HdrInvalid");return false;}
        const Vec3 capped{std::min(pixel.x,65504.0f),std::min(pixel.y,65504.0f),std::min(pixel.z,65504.0f)};
        clamped|=pixel.x!=capped.x||pixel.y!=capped.y||pixel.z!=capped.z;pixel=capped;
    }
    EnvironmentAsset asset;asset.settings=settings;asset.sourceHash=std::move(sourceHash);asset.cookKey=key;
    asset.textures={Texture(settings.irradianceSize,1,true),Texture(settings.cubeSize,Levels(settings.cubeSize),true),Texture(settings.lutSize,1,false)};
    const auto samples=Samples(settings.samples);
    for(std::uint32_t face=0;face<6;++face)
    {
        auto& diffuse=asset.textures[0].subresources[face];
        for(std::uint32_t y=0;y<settings.irradianceSize;++y)for(std::uint32_t x=0;x<settings.irradianceSize;++x)
        {
            const auto n=EnvironmentCubeDirection(face,(float(x)+0.5f)/float(settings.irradianceSize),(float(y)+0.5f)/float(settings.irradianceSize));Vec3 sum{};
            for(const auto sample:samples)
            {
                const double radius=std::sqrt(sample.x),phi=2*pi*sample.y;const auto local=Vec3{float(radius*std::cos(phi)),float(radius*std::sin(phi)),float(std::sqrt(1-sample.x))};
                sum+=SampleEquirectangular(input,Frame(n,local));
            }
            Store(diffuse,size_t(y)*settings.irradianceSize+x,sum*float(pi/settings.samples),clamped);
        }
        auto& spec=asset.textures[1];
        for(std::uint32_t mip=0;mip<spec.mipLevels;++mip)
        {
            const auto size=std::max(1u,spec.size>>mip);auto& pixels=spec.subresources[size_t(face)*spec.mipLevels+mip];const float roughness=float(mip)/float(spec.mipLevels-1);
            for(std::uint32_t y=0;y<size;++y)for(std::uint32_t x=0;x<size;++x)
            {
                const auto n=EnvironmentCubeDirection(face,(float(x)+0.5f)/float(size),(float(y)+0.5f)/float(size));Vec3 sum{};double weight=0;
                if(!mip)sum=SampleEquirectangular(input,n);
                else for(const auto sample:samples)
                {
                    const auto h=Frame(n,Ggx(sample,roughness)),l=Normalize(h*(2*Dot(n,h))-n);const float nl=std::max(Dot(n,l),0.0f);
                    if(nl>0){sum+=SampleEquirectangular(input,l)*nl;weight+=nl;}
                }
                if(mip)sum=weight>1e-12?sum/float(weight):SampleEquirectangular(input,n);
                Store(pixels,size_t(y)*size+x,sum,clamped);
            }
        }
    }
    auto& lut=asset.textures[2].subresources[0];
    for(std::uint32_t y=0;y<settings.lutSize;++y)for(std::uint32_t x=0;x<settings.lutSize;++x)
    {
        const double nv=(double(x)+0.5)/settings.lutSize,rough=(double(y)+0.5)/settings.lutSize,alpha=std::pow(std::max(rough,0.045),2),a2=alpha*alpha;
        const Vec3 v{float(std::sqrt(1-nv*nv)),0,float(nv)};double a=0,b=0;
        for(const auto sample:samples)
        {
            const auto h=Ggx(sample,static_cast<float>(rough)),l=Normalize(h*(2*Dot(v,h))-v);const double nl=std::max(double(l.z),0.0),nh=std::max(double(h.z),0.0),vh=std::max(double(Dot(v,h)),0.0);
            if(nl<=0||nh<=1e-8)continue;
            const double visibility=0.5/std::max(nl*std::sqrt(nv*nv*(1-a2)+a2)+nv*std::sqrt(nl*nl*(1-a2)+a2),1e-8);
            const double weight=4*visibility*nl*vh/nh,fc=std::pow(1-vh,5);a+=(1-fc)*weight;b+=fc*weight;
        }
        Store(lut,size_t(y)*settings.lutSize+x,{float(a/settings.samples),float(b/settings.samples),0},clamped);
    }
    if(!ValidateEnvironment(asset)){asset_io::Diagnostic(diagnostics,"Environment.CookInvalid");return false;}
    if(clamped)asset_io::Diagnostic(diagnostics,"Environment.RangeClamped",{},true);output=std::move(asset);return true;
}
}
