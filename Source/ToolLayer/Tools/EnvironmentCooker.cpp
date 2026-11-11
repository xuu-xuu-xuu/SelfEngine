#include "ResourceLayer/Assets/Environment.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <iostream>
#include <string_view>
#include <Windows.h>

namespace
{
se::FloatImage Sample(bool contrast)
{
    se::FloatImage image{256,128,{}};image.pixels.reserve(size_t(image.width)*image.height);
    const auto key=se::Normalize(se::Vec3{0.3f,0.7f,-0.6f}),fill=se::Normalize(se::Vec3{-0.7f,0.3f,0.5f});
    for(std::uint32_t y=0;y<image.height;++y)for(std::uint32_t x=0;x<image.width;++x)
    {
        const double theta=(double(y)+0.5)/image.height*se::Pi,phi=((double(x)+0.5)/image.width-0.5)*2*se::Pi;
        const se::Vec3 direction{float(std::sin(theta)*std::cos(phi)),float(std::cos(theta)),float(std::sin(theta)*std::sin(phi))};
        const float upper=std::max(direction.y,0.0f),hot=std::pow(std::max(se::Dot(direction,key),0.0f),contrast?64.0f:4.0f),cool=std::pow(std::max(se::Dot(direction,fill),0.0f),8.0f);
        const auto color=contrast?se::Vec3{0.04f,0.06f,0.12f}*(0.2f+upper*0.8f)+se::Vec3{12,10,6}*hot+se::Vec3{0.4f,0.7f,1.8f}*cool
            :se::Vec3{0.12f,0.18f,0.3f}*(0.4f+upper*0.6f)+se::Vec3{0.6f,0.5f,0.35f}*hot;
        image.pixels.push_back(color);
    }
    return image;
}
bool Number(std::wstring_view input,std::uint32_t& value)
{
    if(input.empty()||input.find_first_not_of(L"0123456789")!=std::wstring_view::npos)return false;
    try{const auto parsed=std::stoull(std::wstring(input));if(parsed>UINT32_MAX)return false;value=static_cast<std::uint32_t>(parsed);return true;}catch(...){return false;}
}
}
int wmain(int argc,wchar_t** argv)
{
    // 构建器捕获输出时直接写UTF-8字节；不能改变并行cl共享控制台的代码页。
    DWORD consoleMode=0;if(GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),&consoleMode))SetConsoleOutputCP(CP_UTF8);
    Localization text;std::string error;
    if(!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error))
    {std::cerr<<"EnvironmentCooker could not load localization: "<<error<<'\n';return 1;}
    const auto usage=[&]{Log(LogLevel::Error,text.GetText("EnvironmentCooker.Usage"));return 2;};
    if(argc!=4&&argc!=8)return usage();const std::wstring_view mode=argv[1];std::vector<se::AssetDiagnostic> diagnostics;bool success=false,hit=false;
    if(mode==L"sample"&&argc==4)
    {
        const std::wstring_view kind=argv[2];if(kind!=L"soft"&&kind!=L"contrast")return usage();
        success=se::WriteRadiance(argv[3],Sample(kind==L"contrast"),diagnostics);
    }
    else if(mode==L"cook")
    {
        se::EnvironmentSettings settings;
        if(argc==8&&(!Number(argv[4],settings.cubeSize)||!Number(argv[5],settings.irradianceSize)||!Number(argv[6],settings.lutSize)||!Number(argv[7],settings.samples)))return usage();
        const auto destination=std::filesystem::absolute(argv[3]);
        success=se::CookEnvironment(argv[2],destination,settings,hit,diagnostics);
        if(success)
        {
            se::AssetStore catalog(destination.parent_path(),destination.parent_path()/"Cache");se::AssetId id;
            success=catalog.RegisterSource(destination.filename(),{},id,diagnostics);
        }
    }
    else return usage();
    for(const auto& diagnostic:diagnostics)Log(diagnostic.warning?LogLevel::Warning:LogLevel::Error,text.GetText(diagnostic.key)+": "+diagnostic.detail);
    if(success)Log(LogLevel::Info,text.GetText(hit?"EnvironmentCooker.CacheHit":"EnvironmentCooker.Complete")+": "+se::asset_io::Utf8(argv[3]));
    return success?0:1;
}
