#pragma once
#include "CoreLayer/Math/Math.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace se
{
struct TODParameters
{
    Vec3 sunColor{1,1,1},skyColor{0.02f,0.04f,0.08f},fogColor{0.5f,0.6f,0.7f};
    float sunIntensity=1,fogDensity=0;
    bool operator==(const TODParameters&)const=default;
};
struct TODKeyframe {double time=0;TODParameters value;};
struct TODProfile
{
    std::uint32_t version=1;
    double dayLengthSeconds=600,initialTime=0.25,rate=1,sunrise=0.25,sunset=0.75;
    float azimuthRadians=0,maximumElevationRadians=1.2f;
    std::vector<TODKeyframe> keys{{0,{}}};
};
// 场景持久化配置，不保存运行时钟；SceneManager 保存每个活动场景的配置和资源租约。
struct SceneEnvironmentSettings
{
    std::string todProfile;
    bool preserveTime=false;
    bool operator==(const SceneEnvironmentSettings&)const=default;
};
bool ValidateTODProfile(const TODProfile& profile,std::string& error);
bool ParseTODProfile(std::string_view text,TODProfile& profile,std::string& error);
bool SerializeTODProfile(const TODProfile& profile,std::string& text,std::string& error);
bool ReadTODProfile(const std::filesystem::path& path,TODProfile& profile,std::string& error);
bool SaveTODProfile(const std::filesystem::path& path,const TODProfile& profile,std::string& error);
TODParameters SampleTODProfile(const TODProfile& profile,double time);
}
