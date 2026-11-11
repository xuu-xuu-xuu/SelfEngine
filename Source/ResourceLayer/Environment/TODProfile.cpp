#include "TODProfile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <Windows.h>

namespace se
{
namespace
{
using Json=nlohmann::json;
bool Color(Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&v.x>=0&&v.y>=0&&v.z>=0&&v.x<=65504&&v.y<=65504&&v.z<=65504;}
bool Only(const Json& v,std::initializer_list<std::string_view> keys){if(!v.is_object()||v.size()!=keys.size())return false;for(const auto k:keys)if(!v.contains(k))return false;return true;}
Json Vector(Vec3 v){return Json::array({v.x,v.y,v.z});}
Vec3 Vector(const Json& v){if(!v.is_array()||v.size()!=3)throw std::runtime_error("TOD.InvalidProfile");return {v.at(0).get<float>(),v.at(1).get<float>(),v.at(2).get<float>()};}
Vec3 Blend(Vec3 a,Vec3 b,double t){return {static_cast<float>(a.x+(b.x-a.x)*t),static_cast<float>(a.y+(b.y-a.y)*t),static_cast<float>(a.z+(b.z-a.z)*t)};}
}
bool ValidateTODProfile(const TODProfile& p,std::string& error)
{
    error.clear();
    if(p.version!=1){error="TOD.UnsupportedVersion";return false;}
    if(!std::isfinite(p.dayLengthSeconds)||p.dayLengthSeconds<=0||!std::isfinite(p.rate)||p.rate<0
        ||!std::isfinite(p.initialTime)||p.initialTime<0||p.initialTime>=1||!std::isfinite(p.sunrise)||!std::isfinite(p.sunset)
        ||p.sunrise<0||p.sunrise>=p.sunset||p.sunset>=1||!std::isfinite(p.azimuthRadians)||std::abs(p.azimuthRadians)>1000000
        ||!std::isfinite(p.maximumElevationRadians)||p.maximumElevationRadians<=0||p.maximumElevationRadians>1.5707964f
        ||p.keys.empty()||p.keys.size()>256){error="TOD.InvalidProfile";return false;}
    double last=-1;
    for(const auto& key:p.keys){const auto& v=key.value;if(!std::isfinite(key.time)||key.time<0||key.time>=1||key.time<=last
        ||!Color(v.sunColor)||!Color(v.skyColor)||!Color(v.fogColor)||!std::isfinite(v.sunIntensity)||v.sunIntensity<0||v.sunIntensity>65504
        ||!std::isfinite(v.fogDensity)||v.fogDensity<0||v.fogDensity>1000){error="TOD.InvalidProfile";return false;}last=key.time;}
    return true;
}
bool ParseTODProfile(std::string_view text,TODProfile& profile,std::string& error)
{
    error.clear();if(text.size()>1024*1024){error="TOD.InvalidProfile";return false;}
    try{
        bool duplicate=false;std::vector<std::unordered_set<std::string>> keys;
        const auto j=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t e,Json& v){if(depth>16)throw std::runtime_error("TOD.InvalidProfile");if(e==Json::parse_event_t::object_start)keys.emplace_back();else if(e==Json::parse_event_t::key){if(!keys.back().insert(v.get<std::string>()).second)duplicate=true;}else if(e==Json::parse_event_t::object_end)keys.pop_back();return true;});
        if(duplicate||!Only(j,{"format","version","dayLengthSeconds","initialTime","rate","sunrise","sunset","azimuthRadians","maximumElevationRadians","keys"})||j.at("format")!="SelfEngine.TODProfile"||!j.at("version").is_number_unsigned())throw std::runtime_error("TOD.InvalidProfile");
        TODProfile p;p.version=j.at("version").get<std::uint32_t>();if(j.at("version")!=1){error="TOD.UnsupportedVersion";return false;}
        p.dayLengthSeconds=j.at("dayLengthSeconds").get<double>();p.initialTime=j.at("initialTime").get<double>();p.rate=j.at("rate").get<double>();p.sunrise=j.at("sunrise").get<double>();p.sunset=j.at("sunset").get<double>();p.azimuthRadians=j.at("azimuthRadians").get<float>();p.maximumElevationRadians=j.at("maximumElevationRadians").get<float>();
        p.keys.clear();if(!j.at("keys").is_array()||j.at("keys").size()>256)throw std::runtime_error("TOD.InvalidProfile");
        for(const auto& k:j.at("keys")){if(!Only(k,{"time","sunColor","sunIntensity","skyColor","fogColor","fogDensity"}))throw std::runtime_error("TOD.InvalidProfile");TODKeyframe key;key.time=k.at("time").get<double>();key.value.sunColor=Vector(k.at("sunColor"));key.value.sunIntensity=k.at("sunIntensity").get<float>();key.value.skyColor=Vector(k.at("skyColor"));key.value.fogColor=Vector(k.at("fogColor"));key.value.fogDensity=k.at("fogDensity").get<float>();p.keys.push_back(key);}
        if(!ValidateTODProfile(p,error))return false;profile=std::move(p);return true;
    }catch(const std::exception&){error="TOD.InvalidProfile";return false;}
}
bool SerializeTODProfile(const TODProfile& p,std::string& text,std::string& error)
{
    if(!ValidateTODProfile(p,error))return false;
    Json j{{"format","SelfEngine.TODProfile"},{"version",p.version},{"dayLengthSeconds",p.dayLengthSeconds},{"initialTime",p.initialTime},{"rate",p.rate},{"sunrise",p.sunrise},{"sunset",p.sunset},{"azimuthRadians",p.azimuthRadians},{"maximumElevationRadians",p.maximumElevationRadians},{"keys",Json::array()}};
    for(const auto& k:p.keys)j["keys"].push_back({{"time",k.time},{"sunColor",Vector(k.value.sunColor)},{"sunIntensity",k.value.sunIntensity},{"skyColor",Vector(k.value.skyColor)},{"fogColor",Vector(k.value.fogColor)},{"fogDensity",k.value.fogDensity}});
    text=j.dump(2)+"\n";return true;
}
bool ReadTODProfile(const std::filesystem::path& path,TODProfile& p,std::string& error)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file||file.tellg()<0||file.tellg()>1024*1024){error="TOD.ReadFailed";return false;}std::string text(static_cast<std::size_t>(file.tellg()),'\0');file.seekg(0);if(!file.read(text.data(),static_cast<std::streamsize>(text.size()))){error="TOD.ReadFailed";return false;}return ParseTODProfile(text,p,error);
}
bool SaveTODProfile(const std::filesystem::path& path,const TODProfile& p,std::string& error)
{
    std::string text;if(!SerializeTODProfile(p,text,error))return false;
    // 独占临时文件，完整写入后同目录替换；失败不会破坏上一有效资源。
    auto temporary=path;temporary+=L".tod-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE){error="TOD.WriteFailed";return false;}
    DWORD written=0;const bool complete=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(file);CloseHandle(file);
    if(!complete||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());error="TOD.WriteFailed";return false;}return true;
}
TODParameters SampleTODProfile(const TODProfile& p,double time)
{
    time-=std::floor(time);const auto next=std::upper_bound(p.keys.begin(),p.keys.end(),time,[](double t,const auto& key){return t<key.time;});
    const auto& b=next==p.keys.end()?p.keys.front():*next;const auto& a=next==p.keys.begin()?p.keys.back():*(next-1);
    double begin=a.time,end=b.time;if(next==p.keys.begin())begin-=1;if(next==p.keys.end())end+=1;const double t=p.keys.size()==1?0:(time-begin)/(end-begin);
    TODParameters value;value.sunColor=Blend(a.value.sunColor,b.value.sunColor,t);value.skyColor=Blend(a.value.skyColor,b.value.skyColor,t);value.fogColor=Blend(a.value.fogColor,b.value.fogColor,t);value.sunIntensity=static_cast<float>(a.value.sunIntensity+(b.value.sunIntensity-a.value.sunIntensity)*t);value.fogDensity=static_cast<float>(a.value.fogDensity+(b.value.fogDensity-a.value.fogDensity)*t);return value;
}
}
