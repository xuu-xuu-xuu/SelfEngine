#include "ToolLayer/Editor/EditorWorker.h"
#include <filesystem>
#include <iostream>
#include <string>

int wmain(int argc,wchar_t** argv)
{
    se::EditorWorkerOptions options;
    options.projectRoot=std::filesystem::current_path();
    const auto usage=[](){
        // 参数验证发生在语言表加载之前，使用独立 ASCII 回退。
        std::cerr<<"Usage: EditorSandbox [--project DIR] [--demo | --scene FILE] [--language zh-CN|en-US] [--hidden --frames N] [--capture FILE]\n";
        return 2;
    };
    for(int index=1;index<argc;++index)
    {
        const std::wstring argument=argv[index];
        if(argument==L"--project"&&index+1<argc)options.projectRoot=argv[++index];
        else if(argument==L"--hidden")options.hidden=true;
        else if(argument==L"--demo")options.demo=true;
        else if(argument==L"--scene"&&index+1<argc)options.scene=argv[++index];
        else if(argument==L"--capture"&&index+1<argc)options.capture=argv[++index];
        else if(argument==L"--language"&&index+1<argc)
        {
            const std::wstring language=argv[++index];
            if(language==L"zh-CN")options.locale="zh-CN";else if(language==L"en-US")options.locale="en-US";else return usage();
        }
        else if(argument==L"--frames"&&index+1<argc)
        {
            const std::wstring value=argv[++index];
            if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos)return usage();
            try{std::size_t consumed=0;options.frames=std::stoull(value,&consumed);if(consumed!=value.size()||options.frames==0||options.frames>1'000'000)return usage();}
            catch(...){return usage();}
        }
        else return usage();
    }
    if(options.hidden&&options.frames==0)return usage();
    if(options.demo&&!options.scene.empty())return usage();
    return se::RunEditorWorker(options);
}
