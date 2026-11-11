#include "Environment.h"
#include "AssetIO.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>

namespace se
{
namespace
{
using Json=nlohmann::json;
constexpr std::uint8_t magic[]{'S','E','E','N','V','0','1',0};
Json Options(const EnvironmentSettings& settings)
{return {{"cube",settings.cubeSize},{"irradiance",settings.irradianceSize},{"lut",settings.lutSize},{"samples",settings.samples}};}
void U32(std::vector<std::uint8_t>& bytes,std::uint32_t value){for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>(value>>shift));}
std::uint32_t U32(const std::uint8_t* bytes){return std::uint32_t(bytes[0])|(std::uint32_t(bytes[1])<<8)|(std::uint32_t(bytes[2])<<16)|(std::uint32_t(bytes[3])<<24);}
bool Invalid(std::vector<AssetDiagnostic>& diagnostics){asset_io::Diagnostic(diagnostics,"Environment.ContainerInvalid");return false;}
}
bool SaveEnvironment(const std::filesystem::path& path,const EnvironmentAsset& asset,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateEnvironment(asset))return Invalid(diagnostics);
    Json metadata{{"format","SelfEngine.Environment"},{"schema",1},{"algorithm",EnvironmentCookerVersion},{"source_sha256",asset.sourceHash},
        {"cook_key",asset.cookKey},{"options",Options(asset.settings)},{"textures",Json::array()}};
    const char* names[]{"irradiance","specular","brdf"};std::array<std::vector<std::uint8_t>,3> payloads;
    for(size_t index=0;index<3;++index)
    {
        const auto& texture=asset.textures[index];auto& payload=payloads[index];
        for(const auto& subresource:texture.subresources)for(const auto value:subresource)
        {payload.push_back(static_cast<std::uint8_t>(value));payload.push_back(static_cast<std::uint8_t>(value>>8));}
        const auto hash=Sha256(payload);if(hash.empty())return Invalid(diagnostics);
        metadata["textures"].push_back({{"name",names[index]},{"size",texture.size},{"mips",texture.mipLevels},{"cube",texture.cube},{"bytes",payload.size()},{"sha256",hash}});
    }
    const auto text=metadata.dump(2);if(text.size()>65536)return Invalid(diagnostics);
    std::vector<std::uint8_t> bytes(std::begin(magic),std::end(magic));U32(bytes,static_cast<std::uint32_t>(text.size()));bytes.insert(bytes.end(),text.begin(),text.end());
    for(const auto& payload:payloads)bytes.insert(bytes.end(),payload.begin(),payload.end());
    if(bytes.size()>asset_io::MaxFileBytes)return Invalid(diagnostics);
    auto manifest=path;manifest+=".json";
    // 容器自带清单，旁路 JSON 仅供审阅；容器为运行时唯一权威。
    if(!asset_io::WriteAtomic(manifest,std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()))||!asset_io::WriteAtomic(path,bytes))
    {asset_io::Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(path));return false;}return true;
}
bool DecodeEnvironment(std::span<const std::uint8_t> bytes,EnvironmentAsset& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(bytes.size()>asset_io::MaxFileBytes||bytes.size()<12||!std::equal(std::begin(magic),std::end(magic),bytes.begin()))return Invalid(diagnostics);
    const auto length=U32(bytes.data()+8);if(!length||length>65536||std::uint64_t(length)+12>bytes.size())return Invalid(diagnostics);
    try
    {
        const auto metadata=Json::parse(bytes.begin()+12,bytes.begin()+12+length);
        if(metadata.at("format")!="SelfEngine.Environment"||metadata.at("schema")!=1||metadata.at("algorithm")!=EnvironmentCookerVersion
            ||!metadata.at("textures").is_array()||metadata.at("textures").size()!=3)return Invalid(diagnostics);
        EnvironmentAsset asset;asset.sourceHash=metadata.at("source_sha256").get<std::string>();asset.cookKey=metadata.at("cook_key").get<std::string>();const auto& options=metadata.at("options");
        for(const auto* key:{"cube","irradiance","lut","samples"})if(!options.at(key).is_number_unsigned()||options.at(key).get<std::uint64_t>()>UINT32_MAX)return Invalid(diagnostics);
        asset.settings={options.at("cube").get<std::uint32_t>(),options.at("irradiance").get<std::uint32_t>(),options.at("lut").get<std::uint32_t>(),options.at("samples").get<std::uint32_t>()};
        if(!ValidateEnvironmentSettings(asset.settings)||asset.cookKey.empty()||asset.cookKey!=EnvironmentCookKey(asset.sourceHash,asset.settings))return Invalid(diagnostics);
        size_t cursor=12+length;const char* names[]{"irradiance","specular","brdf"};
        const std::uint32_t sizes[]{asset.settings.irradianceSize,asset.settings.cubeSize,asset.settings.lutSize};
        for(size_t index=0;index<3;++index)
        {
            const auto& description=metadata.at("textures")[index];auto& texture=asset.textures[index];const std::uint32_t levels=index==1?static_cast<std::uint32_t>(std::bit_width(sizes[index])):1;
            for(const auto* key:{"size","mips","bytes"})if(!description.at(key).is_number_unsigned())return Invalid(diagnostics);
            if(description.at("name")!=names[index]||description.at("size")!=sizes[index]||description.at("mips")!=levels||description.at("cube")!=(index<2))return Invalid(diagnostics);
            texture.size=sizes[index];texture.mipLevels=levels;texture.cube=index<2;size_t expectedBytes=0;
            for(std::uint32_t mip=0;mip<levels;++mip){const auto size=std::max(1u,texture.size>>mip);expectedBytes+=size_t(size)*size*8*(texture.cube?6:1);}
            if(description.at("bytes").get<std::uint64_t>()!=expectedBytes||expectedBytes>bytes.size()-cursor)return Invalid(diagnostics);
            const auto payload=std::span(bytes.data()+cursor,expectedBytes);if(description.at("sha256")!=Sha256(payload))return Invalid(diagnostics);
            texture.subresources.resize(size_t(texture.cube?6:1)*levels);
            for(size_t sub=0;sub<texture.subresources.size();++sub)
            {
                const auto size=std::max(1u,texture.size>>static_cast<unsigned>(sub%levels));auto& data=texture.subresources[sub];data.resize(size_t(size)*size*4);
                for(auto& value:data){value=static_cast<std::uint16_t>(std::uint16_t(bytes[cursor])|(std::uint16_t(bytes[cursor+1])<<8));cursor+=2;}
            }
        }
        if(cursor!=bytes.size()||!ValidateEnvironment(asset))return Invalid(diagnostics);output=std::move(asset);return true;
    }
    catch(const Json::exception&){return Invalid(diagnostics);}
}
bool LoadEnvironment(const std::filesystem::path& path,EnvironmentAsset& output,std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path));return false;}
    return DecodeEnvironment(bytes,output,diagnostics);
}
bool CookEnvironment(const std::filesystem::path& source,const std::filesystem::path& destination,const EnvironmentSettings& settings,bool& cacheHit,std::vector<AssetDiagnostic>& diagnostics)
{
    cacheHit=false;std::vector<std::uint8_t> bytes;if(!asset_io::Read(source,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(source));return false;}
    const auto hash=Sha256(bytes),key=EnvironmentCookKey(hash,settings);if(key.empty()){asset_io::Diagnostic(diagnostics,"Environment.CookInvalid");return false;}
    EnvironmentAsset cached;std::vector<AssetDiagnostic> cacheDiagnostics;
    if(LoadEnvironment(destination,cached,cacheDiagnostics)&&cached.cookKey==key){cacheHit=true;return true;}
    FloatImage image;EnvironmentAsset candidate;
    return DecodeRadiance(bytes,image,diagnostics)&&BakeEnvironment(image,hash,settings,candidate,diagnostics)&&SaveEnvironment(destination,candidate,diagnostics);
}
}
