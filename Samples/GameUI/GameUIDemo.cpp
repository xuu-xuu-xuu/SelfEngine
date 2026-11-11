#include "GameUIDemo.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
namespace se
{
bool GameUIDemo::Initialize()
{
    Localization text;std::string error;if(!text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale_+".txt"),error))return false;
    if(transitions_){const bool valid=BuildUiTransitionDemo(ui_,text,transitionDemo_);title_=transitionDemo_.title;return valid;}
    const auto cache=GetExecutableDirectory()/"SampleCache/GameUI";std::error_code ec;std::filesystem::create_directories(cache/"Source",ec);if(ec)return false;
    std::filesystem::copy_file(GetExecutableDirectory()/"Resources/Textures/checker.png",cache/"Source/checker.png",std::filesystem::copy_options::overwrite_existing,ec);if(ec)return false;
    assets_=std::make_unique<AssetStore>(cache/"Source",cache/"Derived");AssetId image;std::vector<AssetDiagnostic> diagnostics;
    if(!assets_->RegisterSource(cache/"Source/checker.png",{},image,diagnostics)||!ui_.SetCanvasReference(960,640))return false;
    bool valid=true;
    const auto create=[&](UiKind kind,UiId parent,UiPoint position,UiPoint size,UiColor color,const char* caption=nullptr)
    {
        const auto id=ui_.Create(kind,parent);UiStyle style;style.offset=position;style.size=size;
        valid=id&&ui_.SetStyle(id,style)&&ui_.SetColor(id,color)&&valid;if(caption)valid=ui_.SetText(id,text.GetText(caption))&&valid;return id;
    };
    const auto root=create(UiKind::Panel,0,{0,0},{960,640},{.015f,.024f,.04f,1});
    title_=create(UiKind::Label,root,{32,28},{896,48},{.8f,.9f,1,1},"UI.TextPage.Title");
    create(UiKind::Label,root,{32,90},{896,48},{.6f,.72f,.86f,1},"UI.TextPage.Subtitle");
    const auto clip=create(UiKind::Panel,root,{32,160},{400,270},{.04f,.065f,.1f,1});
    auto style=ui_.Get(clip)->style;style.clip=true;valid=ui_.SetStyle(clip,style)&&valid;
    const auto sample=create(UiKind::Label,clip,{16,16},{368,200},{.9f,.94f,1,1});
    valid=ui_.SetText(sample,text.GetText("UI.TextPage.English")+"\n"+text.GetText("UI.TextPage.Chinese")+"\n"+text.GetText("UI.TextPage.Mixed")+"\n"+text.GetText("UI.TextPage.Punctuation")+"\n"+text.GetText("UI.TextPage.Clip"))&&valid;
    create(UiKind::Panel,clip,{330,225},{140,90},{.1f,.6f,.9f,.7f});
    const auto picture=create(UiKind::Image,root,{480,160},{240,200},{1,1,1,1});valid=ui_.SetImage(picture,image)&&valid;
    create(UiKind::Panel,root,{600,260},{260,120},{.05f,.6f,.95f,.6f});
    create(UiKind::Label,root,{480,395},{400,80},{.9f,.94f,1,1},"UI.TextPage.Alpha");
    create(UiKind::TextInput,root,{32,485},{896,64},{.9f,.94f,1,1},"UI.TextPage.Input");
    create(UiKind::Label,root,{32,580},{896,40},{.6f,.72f,.86f,1},"UI.TextPage.Footer");
    return valid;
}
ApplicationServices GameUIDemo::Services()
{
    ApplicationServices services;services.ui=&ui_;services.initialWidth=960;services.initialHeight=640;
    services.windowOpened=[this](HWND){return Initialize();};
    services.update=[this](double){const auto events=ui_.TakeEvents();if(transitions_)UpdateUiTransitionDemo(ui_,transitionDemo_,events);return true;};
    services.extract=[this](FrameData& frame){BuildFrameData(world_,&ui_,nullptr,frame);frame.assets=assets_.get();for(auto& visual:frame.ui)if(visual.id==title_)visual.fontSize=28*ui_.CanvasScale();};
    return services;
}
}
