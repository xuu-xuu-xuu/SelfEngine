#include "ToolLayer/Editor/EditorLauncher.h"
#include "ToolLayer/Editor/EditorWorker.h"
#include "PlatformLayer/Windows/ProcessDiagnostics.h"
#include "CoreLayer/Log.h"
#include <Windows.h>
#include <shellapi.h>
#include <cstdio>
#include <io.h>
#include <iostream>
#include <string>

namespace
{
// 开发者在源码中改为 true 并重新构建，即可开启原生调试控制台。
constexpr bool ShowDebugConsole = false;

void ConnectConsoleStream(FILE* stream,const char* device,const char* mode)
{
    const int descriptor=_fileno(stream);
    if(descriptor>=0)
    {
        const auto handle=reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
        const auto type=GetFileType(handle);
        // 自动化和 Worker 的管道/文件重定向需要继续写入原目标。
        if(type==FILE_TYPE_PIPE || type==FILE_TYPE_DISK)return;
    }
    FILE* connected=nullptr;
    freopen_s(&connected,device,mode,stream);
}

void ConfigureDebugConsole()
{
    if constexpr(ShowDebugConsole)
    {
        if(!GetConsoleCP() && !AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole())return;
        ConnectConsoleStream(stdin,"CONIN$","r");
        ConnectConsoleStream(stdout,"CONOUT$","w");
        ConnectConsoleStream(stderr,"CONOUT$","w");
    }
}

std::wstring Wide(const std::string& text)
{
    if(text.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(size<=0)return {};
    std::wstring result(static_cast<size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size);
    return result;
}
int Usage()
{
    // 参数错误发生在语言表与窗口创建之前，保留独立 ASCII 回退。
    std::cerr<<"Usage: FlareEditor [--project DIR|project.json] [--language zh-CN|en-US] [--safe-mode]\n";
    return 2;
}
}

int RunEditor(int argc,wchar_t** argv)
{
    InitializeProcessDiagnostics("editor");
    LogCheckpoint("Editor.Entry");
    LogCheckpoint("Editor.Console",std::string("enabled=")+(ShowDebugConsole?"1":"0")+
        " attached="+(GetConsoleCP()?"1":"0")+" window="+(GetConsoleWindow()?"1":"0"));
    se::EditorLaunchOptions options;
    std::string error;
    if(!se::ParseEditorLaunchOptions(argc,argv,options,error))
    {
        std::cerr<<"[Error] "<<error<<"\n";
        if(options.uiErrors)
        {
            const std::wstring detail=L"FlareEditor: "+Wide(error)+L"\n\nUsage: FlareEditor [--project DIR|project.json] [--language zh-CN|en-US] [--safe-mode]";
            MessageBoxW(nullptr,detail.c_str(),L"FlareEditor",MB_ICONERROR|MB_OK);
        }
        return Usage();
    }
    if(options.worker)
    {
        se::EditorWorkerOptions worker;
        worker.projectRoot=options.project;
        worker.scene=options.scene;
        worker.capture=options.capture;
        worker.locale=options.locale;
        worker.editorUi=options.editorUi;
        worker.frames=options.frames;
        worker.hidden=options.hidden;
        worker.demo=options.demo;
        worker.smokeExportReport=options.smokeExportReport;
        worker.smokeExportName=options.smokeExportName;
        worker.smokeCancelMilliseconds=options.smokeCancelMilliseconds;
        worker.smokePlayback=options.smokePlayback;
        worker.smokeWorkflow=options.smokeWorkflow;
        return se::RunEditorWorker(worker);
    }
    return se::RunEditorLauncher(options);
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int)
{
    ConfigureDebugConsole();
    int argc=0;
    const auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return Usage();
    const int result=RunEditor(argc,argv);
    LocalFree(argv);
    return result;
}
