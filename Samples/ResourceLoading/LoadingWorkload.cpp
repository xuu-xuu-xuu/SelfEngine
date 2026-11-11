#include "LoadingWorkload.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace se
{
namespace
{
using Json=nlohmann::json;
std::string Name(std::size_t index){std::ostringstream stream;stream<<"texture-"<<std::setw(2)<<std::setfill('0')<<index<<".bmp";return stream.str();}
bool Save(const std::filesystem::path& path,const Json& data)
{const auto text=data.dump(2)+"\n";return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
std::vector<std::uint8_t> Image(std::uint32_t width,std::uint32_t height,std::uint32_t index,std::uint32_t seed)
{
    const size_t pitch=(size_t(width)*3+3)/4*4;std::vector<std::uint8_t> bytes(54+pitch*height);
    const auto u16=[&](size_t at,std::uint32_t value){bytes[at]=std::uint8_t(value);bytes[at+1]=std::uint8_t(value>>8);};const auto u32=[&](size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=std::uint8_t(value>>(i*8));};
    bytes[0]='B';bytes[1]='M';u32(2,static_cast<std::uint32_t>(bytes.size()));u32(10,54);u32(14,40);u32(18,width);u32(22,height);u16(26,1);u16(28,24);u32(34,static_cast<std::uint32_t>(pitch*height));
    for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x)
    {
        const auto at=54+size_t(y)*pitch+x*3;const std::uint32_t checker=((x/31+y/17+index)&1)?151:47;
        bytes[at]=std::uint8_t(checker+index*29+(seed&15));bytes[at+1]=std::uint8_t(checker+index*17+((seed>>4)&15));bytes[at+2]=std::uint8_t(checker+index*7+((seed>>8)&15));
    }
    return bytes;
}
Json Identity(std::uint32_t width,std::uint32_t height,std::uint32_t seed,const Json& files)
{return Json{{"algorithm","SelfEngine.ResourceLoading.1"},{"size",{width,height}},{"seed",seed},{"files",files}};}
}
bool CreateLoadingWorkload(const std::filesystem::path& requested,bool light,std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        const auto root=std::filesystem::absolute(requested);if(std::filesystem::exists(root))throw std::runtime_error("output.exists");std::filesystem::create_directories(root/"Assets");
        const std::uint32_t count=light?4:16,width=light?65:1025,height=light?33:513,seed=424242;AssetStore assets(root/"Assets",root/"Cache");Json files=Json::array(),entries=Json::array();
        for(std::uint32_t index=0;index<count;++index)
        {
            const auto name=Name(index);const auto bytes=Image(width,height,index,seed);if(!asset_io::WriteAtomic(root/"Assets"/name,bytes))throw std::runtime_error("workload.write");AssetId id;
            if(!assets.RegisterSource(name,{},id,diagnostics))return false;const auto hash=Sha256(bytes);files.push_back({{"file",name},{"sha256",hash}});entries.push_back({{"file",name},{"sha256",hash},{"id",id}});
        }
        const auto digest=Sha256(Identity(width,height,seed,files).dump());
        if(!Save(root/"workload.json",{{"schema",1},{"algorithm","SelfEngine.ResourceLoading.1"},{"light",light},{"width",width},{"height",height},{"seed",seed},{"count",count},{"content_sha256",digest},{"images",entries}}))throw std::runtime_error("workload.manifest");
        return true;
    }
    catch(const std::exception& error){asset_io::Diagnostic(diagnostics,"Sample.LoadingWorkloadInvalid",error.what());return false;}
}
bool ReadLoadingWorkload(const std::filesystem::path& requested,LoadingWorkload& output,std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        LoadingWorkload candidate;candidate.root=std::filesystem::weakly_canonical(requested);std::vector<std::uint8_t> bytes;
        if(!asset_io::Read(candidate.root/"workload.json",bytes)||bytes.size()>64*1024)throw std::runtime_error("workload.manifest");const auto root=Json::parse(bytes);
        if(root.at("schema")!=1||root.at("algorithm")!="SelfEngine.ResourceLoading.1")throw std::runtime_error("workload.version");candidate.light=root.at("light").get<bool>();candidate.width=root.at("width").get<std::uint32_t>();candidate.height=root.at("height").get<std::uint32_t>();candidate.seed=root.at("seed").get<std::uint32_t>();
        const std::uint32_t count=candidate.light?4:16;
        if(candidate.width!=(candidate.light?65u:1025u)||candidate.height!=(candidate.light?33u:513u)||candidate.seed!=424242||root.at("count")!=count||root.at("images").size()!=count)throw std::runtime_error("workload.fixed inputs");
        AssetStore assets(candidate.root/"Assets",candidate.root/"Cache");if(!assets.Scan(diagnostics))return false;Json files=Json::array();
        for(std::uint32_t index=0;index<count;++index)
        {
            const auto& value=root.at("images")[index];const auto name=Name(index),id=value.at("id").get<std::string>();if(value.at("file")!=name)throw std::runtime_error("workload.path");
            if(std::filesystem::weakly_canonical(assets.Resolve(id))!=std::filesystem::weakly_canonical(candidate.root/"Assets"/name)||!asset_io::Read(candidate.root/"Assets"/name,bytes))throw std::runtime_error("workload.catalog");
            const auto hash=Sha256(bytes);if(value.at("sha256")!=hash)throw std::runtime_error("workload.digest");files.push_back({{"file",name},{"sha256",hash}});candidate.images.push_back(id);
        }
        candidate.digest=Sha256(Identity(candidate.width,candidate.height,candidate.seed,files).dump());if(root.at("content_sha256")!=candidate.digest)throw std::runtime_error("workload.content identity");output=std::move(candidate);return true;
    }
    catch(const std::exception& error){asset_io::Diagnostic(diagnostics,"Sample.LoadingWorkloadInvalid",error.what());return false;}
}
}
