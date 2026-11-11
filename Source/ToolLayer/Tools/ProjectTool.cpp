#include "ToolLayer/Tools/Project.h"
#include <charconv>
#include <iostream>

int wmain(int argc,wchar_t** argv)
{
    // 独立命令行启动回退；具体失败使用可本地化的稳定诊断键。
    if(argc<3)
    {std::cerr<<"ProjectTool create <template-dir> <new-project-dir>\nProjectTool validate <project.json>\nProjectTool dependencies <project.json> <output.json>\nProjectTool replay <project.json> <output-dir> [steps] [--no-profile]\n";return 2;}
    const std::wstring command=argv[1];std::vector<se::ProjectDiagnostic> diagnostics;bool success=false;
    if(command==L"create"&&argc==4)success=se::CreateProject(argv[2],argv[3],diagnostics);
    else if(command==L"validate"&&argc==3)
    {se::ProjectValidation result;success=se::ValidateProject(argv[2],result,diagnostics);if(success)std::cout<<"Validated entities="<<result.entities<<" uiNodes="<<result.uiNodes<<" dependencies="<<result.dependencies.size()<<'\n';}
    else if(command==L"dependencies"&&argc==4)success=se::ExportProjectDependencies(argv[2],argv[3],diagnostics);
    else if(command==L"replay"&&argc>=4&&argc<=6)
    {
        std::uint32_t steps=0;bool profile=true;bool valid=true;
        for(int i=4;i<argc;++i)
        {
            const std::wstring value=argv[i];if(value==L"--no-profile"){profile=false;continue;}
            if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos){valid=false;break;}
            try{const auto number=std::stoull(value);if(number==0||number>36000){valid=false;break;}steps=static_cast<std::uint32_t>(number);}catch(...){valid=false;break;}
        }
        if(!valid){std::cerr<<"Project.InvalidReplay\n";return 2;}
        se::ReplayReport report;success=se::ReplayProject(argv[2],argv[3],report,diagnostics,steps,profile);
        if(success)std::cout<<"Replayed steps="<<report.steps<<" stateSha256="<<report.stateHash<<'\n';
    }
    else {std::cerr<<"Invalid command arguments\n";return 2;}
    for(const auto& diagnostic:diagnostics)std::cerr<<diagnostic.key<<": "<<diagnostic.detail<<'\n';
    return success?0:1;
}
