#include "ToolLayer/Editor/EditorLauncher.h"
#include "ToolLayer/Editor/EditorWorker.h"
#include <Windows.h>
#include <iostream>
#include <string>

namespace
{
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
    std::cerr<<"Usage: SelfEngineEditor [--project DIR|project.json] [--language zh-CN|en-US] [--safe-mode]\n";
    return 2;
}
}

int wmain(int argc,wchar_t** argv)
{
    se::EditorLaunchOptions options;
    std::string error;
    if(!se::ParseEditorLaunchOptions(argc,argv,options,error))
    {
        std::cerr<<"[Error] "<<error<<"\n";
        if(options.uiErrors)
        {
            const std::wstring detail=L"SelfEngineEditor: "+Wide(error)+L"\n\nUsage: SelfEngineEditor [--project DIR|project.json] [--language zh-CN|en-US] [--safe-mode]";
            MessageBoxW(nullptr,detail.c_str(),L"SelfEngineEditor",MB_ICONERROR|MB_OK);
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
