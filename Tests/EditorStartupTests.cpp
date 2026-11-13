#include "ToolLayer/Editor/EditorLoadingScreen.h"
#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorProcessRunner.h"
#include "PlatformLayer/Windows/AppBranding.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "FunctionLayer/Rendering/Renderer.h"
#include "CoreLayer/Localization.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>

using namespace se;
namespace
{
void Check(bool value,const char* detail){if(!value)throw std::runtime_error(detail);}
struct CaptureImage
{
    int width,height;
    std::vector<DWORD> pixels;
    DWORD At(int x,int y) const {return pixels[(height-1-y)*width+x];}
};
CaptureImage Capture(HWND window,const std::filesystem::path& path={})
{
    RECT area{};Check(GetClientRect(window,&area)!=FALSE,"splash rectangle");
    const auto screen=GetDC(window),dc=CreateCompatibleDC(screen);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=area.right;
    info.bmiHeader.biHeight=area.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;const auto image=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    Check(image&&pixels,"splash capture buffer");const auto previous=SelectObject(dc,image);
    SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);GdiFlush();
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    const DWORD bytes=area.right*area.bottom*4;header.bfSize=header.bfOffBits+bytes;
    CaptureImage result{area.right,area.bottom,std::vector<DWORD>(bytes/sizeof(DWORD))};
    std::memcpy(result.pixels.data(),pixels,bytes);
    SelectObject(dc,previous);DeleteObject(image);DeleteDC(dc);ReleaseDC(window,screen);
    if(!path.empty())
    {
        std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(&header),sizeof(header));
        file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
        file.write(reinterpret_cast<const char*>(result.pixels.data()),bytes);Check(bool(file),"splash capture file");
    }
    return result;
}

void RibbonProgress(EditorLoadingScreen& screen)
{
    const auto window=screen.Window();const auto dpi=GetDpiForWindow(window);
    const auto n=[dpi](int value){return MulDiv(value,dpi,96);};
    ReportStartup(StartupStage::BuildScripts);
    const auto a=Capture(window);Sleep(160);const auto b=Capture(window);
    Check(screen.Snapshot().percent==45,"ribbon motion cannot advance startup");
    ReportStartup(StartupStage::Renderer);const auto c=Capture(window);
    int early=0,late=0,changed=0,future=0;
    const auto gold=[](DWORD p){return int((p>>16)&255)-int(p&255)>24;};
    const int w=a.width-2*n(40),edge45=n(40)+MulDiv(w,45,100),edge90=n(40)+MulDiv(w,90,100);
    for(int y=a.height-n(143);y<a.height-n(79);++y)
        for(int x=n(40);x<a.width-n(40);++x)
        {
            if(x<edge45-2 && gold(a.At(x,y)))++early;
            if(x>edge45+2 && gold(a.At(x,y)))++future;
            if(x>edge45+2 && x<edge90-2 && gold(c.At(x,y)))++late;
            if(x>edge90+2 && gold(c.At(x,y)))++future;
            if(a.At(x,y)!=b.At(x,y))++changed;
        }
    Check(early>50 && late>50 && future==0,"gold ribbon fills only the completed span");
    BOOL motion=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&motion,0);
    Check(motion?changed>50:changed==0,"ribbon follows the system animation preference");
}
class ProbeRenderer final:public Renderer
{
public:
    EditorLoadingScreen& loading;bool failInit=false,failFrame=false,visibleAtShutdown=false,cancelInFrame=false;int frames=0;HWND window=nullptr;
    explicit ProbeRenderer(EditorLoadingScreen& screen):loading(screen){}
    bool Initialize(const RenderSurface& surface,const Localization&) override
    {window=surface.window;Check(loading.Snapshot().stage==StartupStage::Renderer&&loading.Snapshot().percent==90,"renderer entry reports real 90 percent checkpoint");return !failInit;}
    RenderResult Render(const RenderFrame&,const Localization&) override
    {++frames;Check(!IsWindowVisible(window),"main window stays hidden until first frame is ready");Check(loading.Snapshot().stage==StartupStage::FirstFrame&&loading.Snapshot().percent==97,"100 percent cannot precede successful render");if(cancelInFrame)SendMessageW(loading.Window(),WM_CLOSE,0,0);return failFrame?RenderResult::Failed:RenderResult::Success;}
    void Shutdown() noexcept override {visibleAtShutdown=IsWindowVisible(window)!=FALSE;}
};
void Channel(const std::filesystem::path& output)
{
    StartupProgressChannel channel;Check(channel.Valid()&&StartupProgressActive(),"startup mapping is active");
    Check(channel.Read().percent==5,"project starts at checkpoint 5");Sleep(80);
    Check(channel.Read().percent==5,"time alone never advances startup progress");
    Check(ReportStartup(StartupStage::Renderer)&&channel.Read().percent==90,"real stage advances progress");
    ReportStartup(StartupStage::Assets);Check(channel.Read().stage==StartupStage::Renderer&&channel.Read().percent==90,"late checkpoints never regress progress");
    channel.Reset();channel.Cancel();Check(!ReportStartup(StartupStage::Ready)&&channel.Read().percent==5,"cancellation never reports completion");
    channel.Reset();
    wchar_t executable[32768]{};Check(GetModuleFileNameW(nullptr,executable,32768)>0,"test executable path");
    ProcessRunResult child;
    Check(RunEditorProcess(executable,{L"--child"},std::filesystem::current_path(),output/"child.log",nullptr,child),"cross-process progress child executes");
    Check(child.started&&child.exitCode==0&&channel.Read().stage==StartupStage::Ready&&channel.Read().percent==100,"controlled child reports to inherited unique channel");
    Check(!StartupProgressActive(),"completed channel does not hide subsequent windows");
}
void FirstFrame(const Localization& text,const std::filesystem::path& output)
{
    const auto project=output/"project";std::filesystem::create_directories(project/"Assets");
    EditorLoadingScreen loading(text,project,true);ProbeRenderer renderer(loading);
    Check(RunEditorApp(renderer,"en-US",project,true,1)==0&&renderer.frames==1,"finite CPU editor host renders");
    Check(loading.Snapshot().stage==StartupStage::Ready&&loading.Snapshot().percent==100,"only completed first frame reaches 100 percent");
    loading.Restart();ProbeRenderer initFailure(loading);initFailure.failInit=true;
    Check(RunEditorApp(initFailure,"en-US",project,true,1)==1&&loading.Snapshot().percent==90,"initialization failure remains incomplete");
    loading.Restart();ProbeRenderer frameFailure(loading);frameFailure.failFrame=true;
    Check(RunEditorApp(frameFailure,"en-US",project,true,1)==1&&loading.Snapshot().percent==97,"first-frame failure remains incomplete");
    loading.Restart();SetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",L"1");ProbeRenderer visible(loading);
    Check(RunEditorApp(visible,"en-US",project,false,1)==0&&visible.visibleAtShutdown,"first successful frame hands off to the visible main window");
    SetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",nullptr);
}
void WindowAndBranding(const Localization& text,const std::filesystem::path& output)
{
    SetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",L"1");
    {
        const auto project=output/"cancel-project";std::filesystem::create_directories(project/"Assets");
        EditorLoadingScreen loading(text,project,false);ProbeRenderer renderer(loading);renderer.cancelInFrame=true;
        Check(RunEditorApp(renderer,"en-US",project,false,1)==0&&!renderer.visibleAtShutdown,"cancel during first-frame work never opens the main window");
        Check(loading.Cancelled()&&loading.Snapshot().percent==97,"late cancellation cannot claim completion");
    }
    for(const auto pixels:{16,20,24,32,40,48,64,128,256})Check(AppIcon(pixels)!=nullptr,"embedded multi-size Flare icon");
    DWORD steady=0;
    for(int round=0;round<4;++round)
    {
        EditorLoadingScreen loading(text,std::filesystem::path(L"Flare Project"),false);
        Check(IsWindow(loading.Window())&&GetWindowLongPtrW(loading.Window(),GWLP_USERDATA),"responsive splash owns a real window");
        wchar_t title[128]{};
        Check(GetWindowTextW(loading.Window(),title,128)>0 && std::wstring_view(title).starts_with(L"Flare "),"splash uses the Flare product name");
        Check(SendMessageW(loading.Window(),WM_GETICON,ICON_BIG,0)!=0,"splash has Flare title/taskbar icon");
        if(round==0)
        {
            HWND window=loading.Window();
            const int dpi=GetDpiForWindow(window);
            const auto pos=[dpi](int x,int y){return MAKELPARAM(MulDiv(x,dpi,96),MulDiv(y,dpi,96));};
            RECT before{},after{};GetWindowRect(window,&before);
            SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,pos(60,70));
            SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,pos(90,80));
            SendMessageW(window,WM_LBUTTONUP,0,pos(60,70));
            GetWindowRect(window,&after);
            Check(after.left-before.left==MulDiv(90,dpi,96)-MulDiv(60,dpi,96) &&
                  after.top-before.top==MulDiv(80,dpi,96)-MulDiv(70,dpi,96) && !loading.Cancelled(),"drag repositions splash without cancelling");
            SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,pos(600,418));
            SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,pos(450,418));
            SendMessageW(window,WM_LBUTTONUP,0,pos(450,418));
            RECT released{};GetWindowRect(window,&released);
            Check(EqualRect(&after,&released) && !loading.Cancelled(),"drag out of cancel leaves the window in place");
            SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,pos(600,418));
            SendMessageW(window,WM_LBUTTONUP,0,pos(600,418));
            Check(loading.Cancelled(),"clicking cancel still cancels");
            loading.Restart();
            RibbonProgress(loading);
        }
        ReportStartup(StartupStage::Renderer);if(round==0)Capture(loading.Window(),output/"loading.bmp");
        SendMessageW(loading.Window(),WM_CLOSE,0,0);Check(loading.Cancelled()&&StartupCancelled(),"splash close cancels both owner and host channel");
        loading.Restart();Check(loading.Window()&&!loading.Cancelled()&&loading.Snapshot().percent==5,"rebuild gets a fresh responsive loading state");
        ReportStartup(StartupStage::Ready);const auto limit=GetTickCount64()+3000;
        while(loading.Window()&&GetTickCount64()<limit)Sleep(10);
        Check(!loading.Window(),"ready handoff removes splash window");
        // 图标/文字的系统缓存首次使用会建立资源；后续完整回收周期不得持续增长。
        loading.Restart();ReportStartup(StartupStage::Ready);
    }
    // 单独比较已预热后的重复实例，避免把系统的首次字体缓存误判为泄漏。
    for(int round=0;round<4;++round)
    {
        {EditorLoadingScreen loading(text,std::filesystem::path(L"Flare Project"),false);ReportStartup(StartupStage::Renderer);SendMessageW(loading.Window(),WM_NULL,0,0);}
        const DWORD resources=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        std::cout<<"loading GDI resources after cycle "<<round<<" = "<<resources<<'\n';
        if(round==0)steady=resources;else Check(resources<=steady+2,"loading fonts and capture buffers are released");
    }
    SetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",nullptr);
}
}
int wmain(int argc,wchar_t** argv)
{
    if(argc>1&&std::wstring_view(argv[1])==L"--child")
        return ReportStartup(StartupStage::Assets)&&ReportStartup(StartupStage::FirstFrame)&&ReportStartup(StartupStage::Ready)?0:1;
    try
    {
        const auto output=std::filesystem::absolute(argc>1?argv[1]:L"out/validation/flare-startup/tests")/asset_io::NewId();
        std::filesystem::create_directories(output);Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/zh-CN.txt",error),"startup localization");
        if(argc>2 && std::wstring_view(argv[2])==L"--record-ribbon")
        {
            SetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",L"1");
            EditorLoadingScreen screen(text,L"Flare Studio",false);
            constexpr StartupStage stages[]{StartupStage::Project,StartupStage::PublishHost,StartupStage::BuildScripts,
                StartupStage::Assets,StartupStage::Scene,StartupStage::Renderer,StartupStage::FirstFrame};
            for(int i=0;i<84;++i)
            {
                if(i%12==0)ReportStartup(stages[i/12]);
                Capture(screen.Window(),output/(std::to_string(100+i)+".bmp"));
                Sleep(65);
            }
            ReportStartup(StartupStage::Ready);
            return 0;
        }
        Channel(output);FirstFrame(text,output);WindowAndBranding(text,output);
        std::cout<<"EditorStartup passed: real checkpoints, inherited channel, first-frame gate, failure, cancel, restart, icon and window cleanup.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EditorStartup: "<<error.what()<<'\n';return 1;}
}
