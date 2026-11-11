#include "Gallery/DemoPreset.h"
#include "Materials/MaterialsDemo.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <iostream>
#include <stdexcept>
using namespace se;
namespace {void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply session test-output directory");const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);const auto image=output/"session.ppm";
        SandboxRenderer renderer(SandboxScene::Materials);renderer.SetCaptureOutput(image,2);MaterialsDemo demo("en-US");
        ApplicationServices invalid;invalid.fixedDeltaSeconds=-1;Check(RunSandboxApp("en-US",renderer,false,&invalid)!=0&&!renderer.CaptureSucceeded(),"invalid update step rejected before rendering");
        auto services=demo.Services();services.initialWidth=160;services.initialHeight=120;services.fixedDeltaSeconds=1.0/60;services.maximumFrames=3;
        Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.CaptureSucceeded(),"first captured session");std::vector<std::uint8_t> first;Check(asset_io::Read(image,first)&&first.size()==160*120*3+15,"first actual image dimensions");
        const std::vector<std::uint8_t> marker{1,2,3};Check(asset_io::WriteAtomic(image,marker),"mark prior generated output");
        services=demo.Services();services.initialWidth=160;services.initialHeight=120;services.fixedDeltaSeconds=1.0/60;services.maximumFrames=3;
        Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.CaptureSucceeded(),"same renderer captures again after Shutdown/Initialize");std::vector<std::uint8_t> second;
        Check(asset_io::Read(image,second)&&second==first,"second session rewrites identical capture without stale success state");
        std::cout<<"DemoSessionTests passed (fixed size/step, invalid service, same renderer capture reset and exact repeated image).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"DemoSessionTests: "<<error.what()<<'\n';return 1;}
}
