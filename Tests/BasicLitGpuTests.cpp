#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Rendering/ColorGrading.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Runtime/World.h"
#include "ToolLayer/Editor/SceneDefaults.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<unsigned char> ReadPpm(const std::filesystem::path& path,int& width,int& height)
{
    std::ifstream file(path,std::ios::binary);std::string magic;int maximum=0;
    file>>magic>>width>>height>>maximum;file.get();Check(magic=="P6"&&maximum==255&&width>0&&height>0,"BasicLit PPM header");
    std::vector<unsigned char> rgb(static_cast<size_t>(width)*height*3);file.read(reinterpret_cast<char*>(rgb.data()),static_cast<std::streamsize>(rgb.size()));
    Check(bool(file),"BasicLit PPM body");return rgb;
}
}
int main(int argc,char** argv)
{
    try
    {
        const auto output=std::filesystem::path(argc>1?argv[1]:"out/validation/basiclit");
        std::filesystem::create_directories(output);
        Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"BasicLit localization");
        WindowState window;CreateEngineWindow(window,false);
        struct Cleanup{WindowState& window;~Cleanup(){window.postQuitOnDestroy=false;DestroyEngineWindow(window);}} cleanup{window};
        RECT rect{0,0,320,240};AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"BasicLit window size");
        SandboxRenderer renderer(SandboxScene::BasicLit);
        Check(renderer.Initialize({window.hwnd,320,240},text),"BasicLit pipeline, sky, shadow and sky artifact");
        World world;Check(CreateDefaultScene(world,error),"default scene for BasicLit");
        FrameData data;BuildFrameData(world,nullptr,nullptr,data);
        RenderFrame frame;frame.surface={window.hwnd,320,240};frame.data=&data;frame.updateIndex=1;
        renderer.SetCaptureOutput(output/"basiclit.ppm",3);
        for(std::uint64_t index=1;index<=4;++index){frame.updateIndex=index;const auto result=renderer.Render(frame,text);Check(result==RenderResult::Success,"BasicLit GPU frame");}
        Check(renderer.CaptureSucceeded(),"BasicLit PPM capture");
        int width=0,height=0;const auto rgb=ReadPpm(output/"basiclit.ppm",width,height);
        Check(width==320&&height==240,"BasicLit capture size");
        size_t lit=0,varied=0;for(size_t index=0;index<rgb.size();index+=3)
        {if(rgb[index]+rgb[index+1]+rgb[index+2]>30)++lit;if(rgb[index]!=rgb[0]||rgb[index+1]!=rgb[1]||rgb[index+2]!=rgb[2])++varied;}
        Check(lit>1000&&varied>1000,"BasicLit image contains lit geometry and directional sky variation");
        // G15：编辑器 LUT 生效与热重载。用 `.selut` 覆盖渲染，再改写文件确认 strip 重建；
        // 清空路径后回到占位纹理，图像应与无 LUT 基线逐字节一致。
        ColorGradingSettings warm;warm.gain[0]=1.2f;warm.gain[1]=0.9f;warm.gain[2]=1.1f;
        ColorLut warmLut;Check(warmLut.Build(33,warm),"editor LUT fixture builds");
        const auto lutPath=output/"editor-lut.selut";
        Check(warmLut.SaveAsset(lutPath,ColorLutColorSpace::Linear,error),"editor LUT fixture writes");
        data.colorLutPath=lutPath;
        renderer.SetCaptureOutput(output/"basiclit-lut.ppm",8);
        for(std::uint64_t index=5;index<=8;++index){frame.updateIndex=index;Check(renderer.Render(frame,text)==RenderResult::Success,"BasicLit LUT frame");}
        Check(renderer.CaptureSucceeded(),"BasicLit LUT capture");
        int lutWidth=0,lutHeight=0;const auto lutRgb=ReadPpm(output/"basiclit-lut.ppm",lutWidth,lutHeight);
        Check(lutWidth==320&&lutHeight==240,"BasicLit LUT capture size");
        size_t lutDifference=0;int lutMax=0;
        for(size_t index=0;index<rgb.size();++index){const int delta=std::abs(int(rgb[index])-int(lutRgb[index]));
            if(delta){++lutDifference;lutMax=std::max(lutMax,delta);}}
        Check(lutDifference>1000,"editor LUT changes the BasicLit image");
        ColorGradingSettings cool;cool.lift[0]=0.02f;cool.gamma[0]=0.8f;cool.gain[0]=1.1f;
        ColorLut coolLut;Check(coolLut.Build(33,cool),"editor LUT reload fixture builds");
        Check(coolLut.SaveAsset(lutPath,ColorLutColorSpace::Linear,error),"editor LUT reload fixture writes");
        const auto lutStamp=std::filesystem::last_write_time(lutPath);
        std::filesystem::last_write_time(lutPath,lutStamp+std::chrono::seconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        renderer.SetCaptureOutput(output/"basiclit-lut-reloaded.ppm",12);
        for(std::uint64_t index=9;index<=12;++index){frame.updateIndex=index;Check(renderer.Render(frame,text)==RenderResult::Success,"BasicLit LUT reload frame");}
        Check(renderer.CaptureSucceeded(),"BasicLit LUT reload capture");
        int reloadWidth=0,reloadHeight=0;const auto reloadRgb=ReadPpm(output/"basiclit-lut-reloaded.ppm",reloadWidth,reloadHeight);
        size_t reloadDifference=0;int reloadMax=0;
        for(size_t index=0;index<lutRgb.size();++index){const int delta=std::abs(int(lutRgb[index])-int(reloadRgb[index]));
            if(delta){++reloadDifference;reloadMax=std::max(reloadMax,delta);}}
        Check(reloadDifference>1000,"editor LUT hot reload changes the BasicLit image");
        // G15.09：默认 Rgba16F strip；用 SELFENGINE_LUT_8BIT=1 重烘同一 LUT，验证两条路径都生效且
        // 只在量化级别上有差异（8 位在暗部产生更大的 sRGB 台阶）。
        Check(SetEnvironmentVariableW(L"SELFENGINE_LUT_8BIT",L"1")!=FALSE,"set 8-bit LUT A/B switch");
        Check(warmLut.SaveAsset(lutPath,ColorLutColorSpace::Linear,error),"8-bit A/B LUT rewrite");
        std::filesystem::last_write_time(lutPath,std::filesystem::last_write_time(lutPath)+std::chrono::seconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        renderer.SetCaptureOutput(output/"basiclit-lut-8bit.ppm",16);
        for(std::uint64_t index=13;index<=16;++index){frame.updateIndex=index;Check(renderer.Render(frame,text)==RenderResult::Success,"BasicLit 8-bit LUT frame");}
        Check(renderer.CaptureSucceeded(),"BasicLit 8-bit LUT capture");
        int eightWidth=0,eightHeight=0;const auto eightRgb=ReadPpm(output/"basiclit-lut-8bit.ppm",eightWidth,eightHeight);
        size_t eightDifference=0;int eightMax=0,formatDifference=0,formatMax=0;
        for(size_t index=0;index<rgb.size();++index)
        {
            const int eightDelta=std::abs(int(rgb[index])-int(eightRgb[index]));
            if(eightDelta){++eightDifference;eightMax=std::max(eightMax,eightDelta);}
            const int formatDelta=std::abs(int(lutRgb[index])-int(eightRgb[index]));
            if(formatDelta){++formatDifference;formatMax=std::max(formatMax,formatDelta);}
        }
        Check(eightDifference>1000,"8-bit LUT path still changes the BasicLit image");
        Check(formatDifference>0&&formatMax<=64,"16F and 8-bit strips differ only at quantization scale");
        Check(SetEnvironmentVariableW(L"SELFENGINE_LUT_8BIT",nullptr)!=FALSE,"clear 8-bit LUT A/B switch");
        data.colorLutPath.clear();
        renderer.SetCaptureOutput(output/"basiclit-no-lut.ppm",20);
        for(std::uint64_t index=17;index<=20;++index){frame.updateIndex=index;Check(renderer.Render(frame,text)==RenderResult::Success,"BasicLit no-LUT frame");}
        Check(renderer.CaptureSucceeded(),"BasicLit no-LUT capture");
        int emptyWidth=0,emptyHeight=0;const auto emptyRgb=ReadPpm(output/"basiclit-no-lut.ppm",emptyWidth,emptyHeight);
        Check(emptyRgb==rgb,"clearing the editor LUT restores the original BasicLit image");
        renderer.Shutdown();
        std::cout<<"BasicLitGpuTests passed (BasicLit PSO, shadow, sky, default scene, editor LUT and hot reload).\n";
        std::cout<<"LUT strips: 16F changed="<<lutDifference<<" max="<<lutMax
            <<", reload changed="<<reloadDifference<<" max="<<reloadMax
            <<", 8-bit changed="<<eightDifference<<" max="<<eightMax
            <<", 16F-vs-8F changed="<<formatDifference<<" max="<<formatMax<<'\n';return 0;
    }
    catch(const std::exception& failure){std::cerr<<"BasicLitGpuTests: "<<failure.what()<<'\n';return 1;}
}
