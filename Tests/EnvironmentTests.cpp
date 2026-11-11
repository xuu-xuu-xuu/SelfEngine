#include "ResourceLayer/Assets/HdrImage.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Environment.h"
#include "CoreLayer/Math/Half.h"
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Half()
{
    for(std::uint32_t bits=0;bits<65536;++bits)
    {
        const auto half=static_cast<std::uint16_t>(bits);const auto value=HalfToFloat(half);
        if(((bits>>10)&31)!=31)Check(FloatToHalf(value)==half,"all finite binary16 values roundtrip exactly including negative zero");
        else Check((bits&1023)?std::isnan(value):std::isinf(value),"half nonfinite class");
    }
    Check(FloatToHalf(1)==0x3c00&&FloatToHalf(65504)==0x7bff&&FloatToHalf(65520)==0x7c00,"half unit, maximum and overflow");
    Check(FloatToHalf(1+std::ldexp(1.0f,-11))==0x3c00&&FloatToHalf(1+std::ldexp(3.0f,-11))==0x3c02,"ties round to nearest even");
    Check(FloatToHalf(std::ldexp(1.0f,-24))==1&&FloatToHalf(std::ldexp(1.0f,-25))==0
        &&FloatToHalf(std::nextafter(std::ldexp(1.0f,-25),1.0f))==1,"smallest subnormal and underflow tie");
    Check(std::signbit(HalfToFloat(0x8000))&&FloatToHalf(-0.0f)==0x8000,"signed zero retained");
}
void Hdr(const std::filesystem::path& output)
{
    std::vector<AssetDiagnostic> diagnostics;FloatImage source;source.width=13;source.height=3;
    for(std::uint32_t row=0;row<source.height;++row)for(std::uint32_t column=0;column<source.width;++column)
        source.pixels.push_back({(float(column)+1)*0.5f,0.25f+float(row)*0.5f,16});
    const auto path=output/"odd-width.hdr";Check(WriteRadiance(path,source,diagnostics),"write deterministic new RLE HDR");
    FloatImage decoded;Check(LoadRadiance(path,decoded,diagnostics)&&decoded.width==13&&decoded.height==3,"read HDR dimensions");
    for(size_t index=0;index<source.pixels.size();++index)
        Check(Length(decoded.pixels[index]-source.pixels[index])<1e-6f,"RGBE linear HDR values and top-row orientation");
    std::vector<std::uint8_t> original;Check(asset_io::Read(path,original),"read original RGBE");
    auto bad=source;bad.pixels[0].x=-1;Check(!WriteRadiance(path,bad,diagnostics),"negative HDR rejected");std::vector<std::uint8_t> preserved;
    Check(asset_io::Read(path,preserved)&&preserved==original,"failed HDR publication preserves previous bytes");
    auto truncated=original;truncated.pop_back();const auto previous=decoded.width;
    Check(!DecodeRadiance(truncated,decoded,diagnostics)&&decoded.width==previous,"truncated channel stream rejected atomically");
    const std::string header="#?RGBE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n";
    std::vector<std::uint8_t> run(header.begin(),header.end());run.insert(run.end(),{2,2,0,8,136,128,136,64,136,32,136,129});
    Check(DecodeRadiance(run,decoded,diagnostics)&&decoded.width==8&&decoded.pixels[7].x==1&&decoded.pixels[7].y==0.5f&&decoded.pixels[7].z==0.25f,"all four RLE repeated channels decoded");
    auto malformed=run;malformed[header.size()+4]=0;Check(!DecodeRadiance(malformed,decoded,diagnostics),"zero-length channel run rejected");
    malformed=run;malformed[header.size()+4]=137;Check(!DecodeRadiance(malformed,decoded,diagnostics),"channel run overflow rejected");
    FloatImage tiny{3,1,{{1,0.5f,0.25f},{0,0,0},{8,4,2}}};
    Check(WriteRadiance(output/"flat.hdr",tiny,diagnostics)&&LoadRadiance(output/"flat.hdr",decoded,diagnostics)&&decoded.width==3&&decoded.pixels[2].x==8,"small flat RGBE input");
    const std::string unsupported="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nEXPOSURE=2\n\n-Y 1 +X 1\n";
    std::vector<std::uint8_t> bytes(unsupported.begin(),unsupported.end());bytes.insert(bytes.end(),{128,128,128,129});
    Check(!DecodeRadiance(bytes,decoded,diagnostics)&&diagnostics.back().key=="Environment.HdrMetadataUnsupported","radiometric metadata is explicitly rejected");
}
std::array<double,2> BrdfOracle(double nv,double rough)
{
    // 独立对入射半球做中点面积积分，不复用 cooker 的 GGX 重要性采样。
    constexpr int vertical=160,azimuth=256;constexpr double pi=3.14159265358979323846;
    const double vx=std::sqrt(1-nv*nv),alpha2=rough*rough*rough*rough;std::array<double,2> result{};
    for(int row=0;row<vertical;++row)for(int column=0;column<azimuth;++column)
    {
        const double nl=(row+0.5)/vertical,phi=2*pi*(column+0.5)/azimuth,sine=std::sqrt(1-nl*nl);
        const double lx=sine*std::cos(phi),ly=sine*std::sin(phi),hx=vx+lx,hy=ly,hz=nv+nl;
        const double inverseLength=1/std::sqrt(hx*hx+hy*hy+hz*hz),nh=hz*inverseLength,vh=(vx*hx+nv*hz)*inverseLength;
        const double denominator=nh*nh*(alpha2-1)+1,distribution=alpha2/(pi*denominator*denominator);
        const double visibility=0.5/(nl*std::sqrt(nv*nv*(1-alpha2)+alpha2)+nv*std::sqrt(nl*nl*(1-alpha2)+alpha2));
        const double fresnel=std::pow(1-vh,5),weight=distribution*visibility*nl*(2*pi/(vertical*azimuth));
        result[0]+=(1-fresnel)*weight;result[1]+=fresnel*weight;
    }
    return result;
}
void Bake(const std::filesystem::path& output)
{
    std::vector<AssetDiagnostic> diagnostics;EnvironmentSettings settings{8,4,8,128};
    Check(ValidateEnvironmentSettings(settings),"bounded bake settings");auto invalid=settings;invalid.cubeSize=7;Check(!ValidateEnvironmentSettings(invalid),"non-power-of-two bake rejected");
    invalid={256,64,256,2048};Check(!ValidateEnvironmentSettings(invalid),"excessive bake work budget rejected");
    const Vec3 axes[]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(std::uint32_t face=0;face<6;++face)Check(Length(EnvironmentCubeDirection(face,0.5f,0.5f)-axes[face])<1e-6f,"cube cardinal direction order");
    Check(Length(EnvironmentCubeDirection(0,1,0.5f)-EnvironmentCubeDirection(5,0,0.5f))<1e-6f,"adjacent cube edge directions agree");
    FloatImage uniform{16,8,std::vector<Vec3>(128,{1,0.5f,0.25f})};const auto source=output/"uniform.hdr",destination=output/"uniform.seenv";
    Check(WriteRadiance(source,uniform,diagnostics),"uniform RGBE source");bool hit=true;
    Check(CookEnvironment(source,destination,settings,hit,diagnostics)&&!hit,"first actual offline cook");
    EnvironmentAsset asset;Check(LoadEnvironment(destination,asset,diagnostics)&&ValidateEnvironment(asset),"standalone container validates");
    Check(asset.textures[0].subresources.size()==6&&asset.textures[1].subresources.size()==24&&asset.textures[2].subresources.size()==1,"cube face/mip and LUT shapes");
    const float color[]{1,0.5f,0.25f};
    for(const auto& subresource:asset.textures[0].subresources)for(size_t pixel=0;pixel<subresource.size();pixel+=4)for(size_t channel=0;channel<3;++channel)
        Check(std::abs(HalfToFloat(subresource[pixel+channel])-Pi*color[channel])<0.002f,"uniform irradiance equals pi times radiance");
    for(const auto& subresource:asset.textures[1].subresources)for(size_t pixel=0;pixel<subresource.size();pixel+=4)for(size_t channel=0;channel<3;++channel)
        Check(HalfToFloat(subresource[pixel+channel])==color[channel],"uniform prefilter is unchanged across every face and roughness mip");
    const auto& lut=asset.textures[2].subresources[0];
    for(size_t pixel=0;pixel<lut.size();pixel+=4)
        Check(HalfToFloat(lut[pixel])>=0&&HalfToFloat(lut[pixel])<1.2f&&HalfToFloat(lut[pixel+1])>=0&&HalfToFloat(lut[pixel+1])<1,"BRDF split-sum terms are finite and bounded");
    double maximumLutError=0;
    for(size_t row:{size_t{4},size_t{7}})for(size_t column:{size_t{1},size_t{4},size_t{7}})
    {
        const auto reference=BrdfOracle((double(column)+0.5)/8,(double(row)+0.5)/8);
        for(size_t channel=0;channel<2;++channel)
        {const auto error=std::abs(HalfToFloat(lut[(row*8+column)*4+channel])-reference[channel]);maximumLutError=std::max(maximumLutError,error);Check(error<0.025,"BRDF LUT agrees with independent hemisphere area integral");}
    }
    std::cout<<"128-sample BRDF LUT maximum absolute error against independent area integral: "<<maximumLutError<<'\n';
    const auto before=std::filesystem::last_write_time(destination);Check(CookEnvironment(source,destination,settings,hit,diagnostics)&&hit&&std::filesystem::last_write_time(destination)==before,"same raw hash/options reuse output without rewrite");
    const auto oldKey=asset.cookKey;settings.samples=64;Check(CookEnvironment(source,destination,settings,hit,diagnostics)&&!hit&&LoadEnvironment(destination,asset,diagnostics)&&asset.cookKey!=oldKey,"sample budget changes cook identity");
    const auto sourceTime=std::filesystem::last_write_time(source);const auto settingsKey=asset.cookKey;uniform.pixels[0]={4,2,1};
    Check(WriteRadiance(source,uniform,diagnostics),"source revision");std::filesystem::last_write_time(source,sourceTime);
    Check(CookEnvironment(source,destination,settings,hit,diagnostics)&&!hit&&LoadEnvironment(destination,asset,diagnostics)&&asset.cookKey!=settingsKey,"source content invalidates despite identical timestamp");
    const auto published=output/"published";std::filesystem::create_directories(published);
    std::filesystem::copy_file(destination,published/"standalone.seenv",std::filesystem::copy_options::overwrite_existing);
    EnvironmentAsset standalone;Check(LoadEnvironment(published/"standalone.seenv",standalone,diagnostics)&&standalone.cookKey==asset.cookKey,"container needs neither original HDR nor adjacent manifest");
    std::vector<std::uint8_t> valid;Check(asset_io::Read(destination,valid),"read baked payload");auto damaged=valid;damaged.back()^=1;
    Check(asset_io::WriteAtomic(output/"corrupt.seenv",damaged)&&!LoadEnvironment(output/"corrupt.seenv",standalone,diagnostics)&&standalone.cookKey==asset.cookKey,"payload corruption rejected atomically");
    auto invalidSettings=settings;invalidSettings.samples=0;
    Check(!CookEnvironment(source,destination,invalidSettings,hit,diagnostics),"bad bake request fails");std::vector<std::uint8_t> preserved;
    Check(asset_io::Read(destination,preserved)&&preserved==valid,"failed cook preserves existing artifact");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply environment test directory");const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);
        Half();Hdr(output);Bake(output);std::cout<<"Environment half, Radiance, offline integrals, content cache and standalone container tests passed; GPU IBL is in progress.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EnvironmentTests: "<<error.what()<<'\n';return 1;}
}
