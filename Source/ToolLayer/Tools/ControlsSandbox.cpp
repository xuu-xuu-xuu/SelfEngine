#include "FunctionLayer/GameUI/ControlsGallery.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Log.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <Windows.h>
#include <iostream>

int wmain(int argc,wchar_t** argv)
{
    std::string locale="zh-CN";bool hidden=false;std::uint64_t frames=0;
    const auto usage=[](){std::cerr<<"Usage: ControlsSandbox [--language zh-CN|en-US] [--hidden --frames N]\n";return 2;};
    for(int index=1;index<argc;++index)
    {
        const std::wstring argument=argv[index];
        if(argument==L"--hidden")hidden=true;
        else if(argument==L"--language"&&index+1<argc)
        {const std::wstring language=argv[++index];if(language==L"zh-CN")locale="zh-CN";else if(language==L"en-US")locale="en-US";else return usage();}
        else if(argument==L"--frames"&&index+1<argc)
        {
            const std::wstring value=argv[++index];if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos)return usage();
            try{std::size_t consumed=0;frames=std::stoull(value,&consumed);if(consumed!=value.size()||frames==0||frames>1'000'000)return usage();}
            catch(...){return usage();}
        }
        else return usage();
    }
    if(hidden&&frames==0)return usage();
    SetConsoleOutputCP(CP_UTF8);
    Localization text;std::string error;
    const auto resources=GetExecutableDirectory()/"Resources";
    if(!text.Load(resources/"Localization"/(locale+".txt"),error))
    {
        Log(LogLevel::Warning,"Language "+locale+": "+error);
        if(locale!="en-US")text.Load(resources/"Localization"/"en-US.txt",error);
    }
    se::UiTree ui;se::ControlsGallery gallery;se::World world;
    const auto cache=GetExecutableDirectory()/"SampleCache/Controls";std::error_code fileError;std::filesystem::create_directories(cache/"Source",fileError);
    if(!fileError)std::filesystem::copy_file(resources/"Textures/checker.png",cache/"Source/checker.png",std::filesystem::copy_options::overwrite_existing,fileError);
    if(fileError){Log(LogLevel::Error,text.GetText("App.ServicesInvalid")+": "+fileError.message());return 1;}
    se::AssetStore assets(cache/"Source",cache/"Derived");se::AssetId image;std::vector<se::AssetDiagnostic> diagnostics;
    if(!assets.RegisterSource(cache/"Source/checker.png",{},image,diagnostics))
    {for(const auto& d:diagnostics)Log(LogLevel::Error,text.GetText(d.key)+": "+d.detail);return 1;}
    if(!se::BuildControlsGallery(ui,text,gallery,image))return 1;
    se::ApplicationServices services;services.resourceDirectory=resources;services.ui=&ui;services.maximumFrames=frames;
    services.update=[&](double){const auto events=ui.TakeEvents();se::UpdateControlsGallery(ui,gallery,events,text);return true;};
    services.extract=[&](se::FrameData& frame){se::BuildFrameData(world,&ui,nullptr,frame);frame.assets=&assets;};
    Log(LogLevel::Info,text.GetText("UI.Gallery.CpuReady"));
    SandboxRenderer renderer;
    return RunSandboxApp(locale,renderer,!hidden,&services);
}
