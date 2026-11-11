#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Rendering/RenderExperiment.h"
#include "T02Examples.h"
#include "Camera/CameraDemo.h"
#include "Materials/MaterialsDemo.h"
#include "HLOD/HlodDemo.h"
#include "Shadows/ShadowsDemo.h"
#include "Gallery/DemoPreset.h"
#include "StaticModel/StaticModelDemo.h"
#include "Scene/SceneDemo.h"
#include "Physics/PhysicsDemo.h"
#include "FirstPerson/FirstPersonDemo.h"
#include "GameUI/GameUIDemo.h"
#include "ToolLayer/Tools/Project.h"
#include "CoreLayer/Profiler.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Log.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <Windows.h>

int wmain(int argc, wchar_t* argv[])
{
    std::string locale = "zh-CN";
    bool runT02Examples = false;
    bool verbose = false;
    bool triangleOnly = false;
    bool cameraTriangle = false;
    bool materials=false;
    bool hlod=false;
    bool lighting=false;
    bool shadows=false;
    bool postProcess=false;
    bool physics=false;
    bool firstPerson=false;
    bool gameUi=false;
    bool uiTransitions=false;
    std::string environment;
    std::string demoName;
    std::filesystem::path capturePath;
    std::filesystem::path uiLayoutPath;
    std::uint64_t captureFrame=0;
    std::string modelFormat;
    std::filesystem::path scenePath,assetRoot,projectPath,profileDirectory;
    // G00：只冻结实验开关，默认 off；G01 起由具体任务消费。
    se::RenderExperiment renderExperiment=se::RenderExperiment::Off;
    bool hidden=false;
    std::uint64_t maximumFrames=0;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        if (argument == L"--language" && index + 1 < argc)
        {
            const std::wstring language=argv[++index];
            if(language==L"zh-CN"){locale="zh-CN";continue;}
            if(language==L"en-US"){locale="en-US";continue;}
        }
        else if (argument == L"--run-t02-examples")
        {
            runT02Examples = true;
            continue;
        }
        else if (argument == L"--verbose")
        {
            verbose = true;
            continue;
        }
        else if (argument == L"--triangle")
        {
            triangleOnly = true;
            if(!cameraTriangle)continue;
        }
        else if(argument==L"--camera-triangle")
        {
            cameraTriangle=true;
            if(!triangleOnly)continue;
        }
        else if(argument==L"--materials"){materials=true;continue;}
        else if(argument==L"--hlod"){hlod=true;continue;}
        else if(argument==L"--lighting"){lighting=true;continue;}
        else if(argument==L"--shadows"){shadows=true;continue;}
        else if(argument==L"--postprocess"){postProcess=true;continue;}
        else if(argument==L"--physics"){physics=true;continue;}
        else if(argument==L"--first-person"){firstPerson=true;continue;}
        else if(argument==L"--game-ui"){gameUi=true;continue;}
        else if(argument==L"--ui-transitions"){uiTransitions=true;continue;}
        else if(argument==L"--ui-layout"&&index+1<argc){uiLayoutPath=argv[++index];if(!uiLayoutPath.empty())continue;}
        else if(argument==L"--demo"&&index+1<argc)
        {const std::wstring name=argv[++index];demoName=name==L"materials"?"materials":name==L"shadows"?"shadows":name==L"postprocess"?"postprocess":"";if(!demoName.empty())continue;}
        else if(argument==L"--capture"&&index+1<argc){capturePath=argv[++index];continue;}
        else if(argument==L"--capture-frame"&&index+1<argc)
        {const std::wstring value=argv[++index];if(!value.empty()&&value.find_first_not_of(L"0123456789")==std::wstring::npos)
            try{captureFrame=std::stoull(value);if(captureFrame&&captureFrame<=1'000'000)continue;}catch(...){}}
        else if(argument==L"--environment"&&index+1<argc)
        {const std::wstring kind=argv[++index];environment=kind==L"soft"?"soft":kind==L"contrast"?"contrast":"";if(!environment.empty())continue;}
        else if(argument==L"--model"&&index+1<argc)
        {
            const std::wstring format=argv[++index];
            modelFormat=format==L"fbx"?"fbx":format==L"glb"?"glb":"";
            if((modelFormat=="glb"||modelFormat=="fbx")&&!triangleOnly&&!cameraTriangle)continue;
        }
        else if(argument==L"--scene"&&index+1<argc){scenePath=argv[++index];continue;}
        else if(argument==L"--assets"&&index+1<argc){assetRoot=argv[++index];continue;}
        else if(argument==L"--project"&&index+1<argc){projectPath=argv[++index];continue;}
        else if(argument==L"--profile"&&index+1<argc){profileDirectory=argv[++index];continue;}
        else if(argument==L"--hidden"){hidden=true;continue;}
        else if(argument==L"--render-experiment"&&index+1<argc)
        {
            const std::wstring value=argv[++index];
            std::string narrow;narrow.reserve(value.size());
            for(const wchar_t character:value){if(character>0x7f){narrow.clear();break;}narrow.push_back(static_cast<char>(character));}
            if(!narrow.empty()&&se::ParseRenderExperiment(narrow,renderExperiment))continue;
        }
        else if(argument==L"--frames"&&index+1<argc)
        {
            const std::wstring value=argv[++index];
            if(!value.empty()&&value.find_first_not_of(L"0123456789")==std::wstring::npos)
                try{maximumFrames=std::stoull(value);if(maximumFrames>0&&maximumFrames<=1'000'000)continue;}catch(...){}
        }
        // 参数检查发生在语言表加载之前，保留独立英文用法提示。
        std::cerr << "Usage: EngineSandbox [--language zh-CN|en-US] [--verbose] [--run-t02-examples] [--triangle|--camera-triangle|--physics|--first-person|--game-ui|--ui-transitions|--materials|--hlod|--lighting|--shadows|--environment soft|contrast|--postprocess|--demo materials|shadows|postprocess|--model glb|fbx|--scene FILE [--ui-layout FILE] [--assets DIR]|--ui-layout FILE [--assets DIR]|--project FILE] [--render-experiment off|g01..g15] [--frames N] [--hidden --frames N] [--profile DIR] [--capture FILE [--capture-frame N]]\n";
        return 2;
    }
    (void)renderExperiment; // G00：开关已校验，行为接入由 G01–G15 各自完成。
    static_assert(sizeof(void*) == 8, "自研引擎需要64位编译器");
    SetConsoleOutputCP(CP_UTF8);
    SetDebugLoggingEnabled(verbose);
    if (verbose || runT02Examples)
        std::cout << "当前工作目录：" << std::filesystem::current_path() << '\n';

    if (runT02Examples)
    {
        const int exampleResult = RunT02Examples();
        if (exampleResult != 0)
        {
            return exampleResult;
        }
    }
    const int sceneModes=(triangleOnly?1:0)+(cameraTriangle?1:0)+(materials?1:0)+(hlod?1:0)+(lighting?1:0)+(shadows?1:0)+(postProcess?1:0)+(physics?1:0)+(firstPerson?1:0)+(gameUi?1:0)+(uiTransitions?1:0)+(!uiLayoutPath.empty()&&scenePath.empty()?1:0)+(!demoName.empty()?1:0)+(!environment.empty()?1:0)+(!modelFormat.empty()?1:0)+(!scenePath.empty()?1:0)+(!projectPath.empty()?1:0);
    if(sceneModes>1||(!assetRoot.empty()&&scenePath.empty()&&uiLayoutPath.empty())||(hidden&&!maximumFrames))return 2;
    std::optional<se::DemoPreset> demoPreset;
    if(!demoName.empty())
    {
        se::DemoPreset preset;std::vector<se::AssetDiagnostic> diagnostics;
        if(!se::LoadDemoPreset(GetExecutableDirectory()/"Resources/Demos"/(demoName+".json"),preset,diagnostics))
        {Localization text;std::string error;text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale+".txt"),error);for(const auto& d:diagnostics)Log(LogLevel::Error,text.GetText(d.key)+": "+d.detail);return 1;}
        demoPreset=preset;materials=preset.scene==se::DemoScene::Materials;shadows=preset.scene==se::DemoScene::Shadows;postProcess=preset.scene==se::DemoScene::PostProcess;
    }
    if(capturePath.empty()&&captureFrame)return 2;
    if(!captureFrame)captureFrame=demoPreset?demoPreset->captureFrame:1;
    if(!capturePath.empty()&&maximumFrames&&maximumFrames<captureFrame)return 2;
    if(!projectPath.empty())
    {
        se::ProjectDescription project;std::vector<se::ProjectDiagnostic> diagnostics;
        if(!se::ReadProject(projectPath,project,diagnostics))
        {for(const auto& d:diagnostics)std::cerr<<d.key<<": "<<d.detail<<'\n';return 1;}
        scenePath=project.entryScene;assetRoot=project.contentRoot;
    }
    const bool sceneMode=physics||firstPerson||!modelFormat.empty()||!scenePath.empty();
    SandboxRenderer renderer(gameUi||uiTransitions||(!uiLayoutPath.empty()&&scenePath.empty())?SandboxScene::Clear:postProcess?SandboxScene::PostProcess:!environment.empty()?SandboxScene::Environment:shadows?SandboxScene::Shadows:lighting?SandboxScene::Lighting:hlod?SandboxScene::Hlod:materials?SandboxScene::Materials:sceneMode?SandboxScene::Scene:triangleOnly||cameraTriangle?SandboxScene::Triangle:SandboxScene::TexturedCube);
    if(!capturePath.empty())renderer.SetCaptureOutput(capturePath,captureFrame);
    se::Profiler profiler;
    profiler.SetEnabled(!profileDirectory.empty());
    if(!profileDirectory.empty())
    {
        std::error_code error;std::filesystem::create_directories(profileDirectory,error);
        if(error){std::cerr<<"Profiler.WriteFailed: "<<error.message()<<'\n';return 1;}
        renderer.SetProfileOutput(profileDirectory);
    }
    const auto run=[&](se::ApplicationServices& services){
        if(demoPreset){services.initialWidth=demoPreset->width;services.initialHeight=demoPreset->height;services.fixedDeltaSeconds=demoPreset->fixedDelta;}
        services.maximumFrames=maximumFrames;services.profiler=profileDirectory.empty()?nullptr:&profiler;
        int result=RunSandboxApp(locale,renderer,!hidden,&services);
        if(!result&&!renderer.CaptureSucceeded())
        {Localization text;std::string error;text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale+".txt"),error);Log(LogLevel::Error,text.GetText("Sample.CaptureMissing"));result=1;}
        if(profileDirectory.empty())return result;
        std::string error;
        if(!profiler.ExportFramesCsv(profileDirectory/"cpu.csv",error)
            ||!profiler.ExportSummaryCsv(profileDirectory/"cpu-summary.csv",error)||!renderer.ProfileExportSucceeded())
        {
            Localization text;std::string loadError;text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale+".txt"),loadError);
            std::cerr<<text.GetText(error.empty()?"Profiler.WriteFailed":error)<<'\n';return 1;
        }
        return result;
    };
    if (triangleOnly){se::ApplicationServices services;return run(services);}
    if(physics){se::PhysicsDemo demo(locale);auto services=demo.Services();return run(services);}
    if(firstPerson){se::FirstPersonDemo demo(locale);auto services=demo.Services();return run(services);}
    if(gameUi||uiTransitions){se::GameUIDemo demo(locale,uiTransitions);auto services=demo.Services();return run(services);}
    if(hlod){se::HlodDemo demo(locale);auto services=demo.Services();return run(services);}
    if(materials||lighting||!environment.empty()||postProcess){se::MaterialsDemo demo(locale,lighting,postProcess?"contrast":environment,postProcess,demoPreset?demoPreset->camera:se::FreeCameraState{{0,-0.5f,9},0,0});auto services=demo.Services();return run(services);}
    if(shadows){se::ShadowsDemo demo(locale,demoPreset?demoPreset->camera:se::FreeCameraState{{0,4.5f,8},0,-0.45f});auto services=demo.Services();return run(services);}
    if(!scenePath.empty()||!uiLayoutPath.empty())
    {
        if(assetRoot.empty())assetRoot=std::filesystem::absolute(scenePath.empty()?uiLayoutPath:scenePath).parent_path();
        se::SceneDemo demo(scenePath,assetRoot,locale,uiLayoutPath);auto services=demo.Services();
        return run(services);
    }
    if(!modelFormat.empty())
    {
        se::StaticModelDemo demo(modelFormat);auto services=demo.Services();
        return run(services);
    }
    se::CameraDemo demo(!cameraTriangle);
    auto services = demo.Services();
    return run(services);
}
