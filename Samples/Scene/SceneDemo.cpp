#include "SceneDemo.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Log.h"
#include "ResourceLayer/Assets/AssetIO.h"

namespace se
{
bool SceneDemo::Load()
{
    Localization text;std::string error;
    const auto directory=GetExecutableDirectory();
    text.Load(directory/"Resources/Localization"/(locale_+".txt"),error);
    World candidate;
    if(!scene_.empty()&&!LoadScene(scene_,candidate,error)){Log(LogLevel::Error,text.GetText(error));return false;}
    UiTree ui;
    if(!uiPath_.empty())
    {
        std::vector<std::uint8_t> bytes;
        if(!asset_io::Read(uiPath_,bytes)){Log(LogLevel::Error,text.GetText("EditorUi.UiReadFailed"));return false;}
        if(!ui.LoadJson(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),error)){Log(LogLevel::Error,text.GetText(error));return false;}
    }
    auto assets=std::make_unique<AssetStore>(assetRoot_,directory/"Cache/SceneSample");
    std::vector<AssetDiagnostic> diagnostics;
    if(!assets->Scan(diagnostics))
    {
        for(const auto& diagnostic:diagnostics)Log(LogLevel::Error,text.GetText(diagnostic.key)+": "+diagnostic.detail);
        return false;
    }
    if(!candidate.EnvironmentSettings().todProfile.empty())
    {
        const auto profile=assets->LoadTODProfile(candidate.EnvironmentSettings().todProfile,diagnostics);
        if(!profile||!environment_.TimeOfDay().ValidateProfile(*profile,environment_.Clock().Step(),error)){Log(LogLevel::Error,text.GetText(error.empty()?"TOD.ResolveFailed":error));return false;}
    }
    environment_.Clear();world_=std::move(candidate);ui_=std::move(ui);assets_=std::move(assets);
    environment_.SetTODResolver([this](const std::string& id,std::string& error){std::vector<AssetDiagnostic> diagnostics;auto profile=assets_->LoadTODProfile(id,diagnostics);if(!profile)error=diagnostics.empty()?"TOD.ResolveFailed":diagnostics.back().key;return profile;});
    return true;
}
ApplicationServices SceneDemo::Services()
{
    ApplicationServices services;
    if(!uiPath_.empty())services.ui=&ui_;
    services.windowOpened=[this](HWND){return Load();};
    services.update=[this](double){ui_.TakeEvents();return true;};
    services.runEnvironmentFrame=[this](double seconds){ui_.TakeEvents();return environment_.RunFrame(seconds);};
    services.extract=[this](FrameData& frame){BuildFrameData(environment_,uiPath_.empty()?nullptr:&ui_,nullptr,frame);frame.assets=assets_.get();};
    services.windowClosing=[this]{assets_.reset();};
    return services;
}
}
