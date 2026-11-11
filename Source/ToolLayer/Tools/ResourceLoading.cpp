#include "ResourceLoading/LoadingExperiment.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include <iostream>
#include <stdexcept>

using namespace se;
int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string error;if(!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error)){std::cerr<<"ResourceLoading localization unavailable\n";return 1;}
    try
    {
        LoadingExperimentConfig config;std::filesystem::path prepare;bool light=false;std::string language="en-US";
        const auto number=[&](const std::wstring& value){size_t used=0;const auto count=std::stoull(value,&used);if(used!=value.size()||count>UINT32_MAX)throw std::invalid_argument("number");return static_cast<std::uint32_t>(count);};
        for(int index=1;index<argc;++index)
        {
            const std::wstring argument=argv[index];if(argument==L"--show"){config.show=true;continue;}if(argument==L"--light"){light=true;continue;}
            if(index+1>=argc)throw std::invalid_argument("arguments");const std::wstring value=argv[++index];
            if(argument==L"--prepare-workload")prepare=std::filesystem::absolute(value);
            else if(argument==L"--workload")config.workload=std::filesystem::absolute(value);
            else if(argument==L"--output")config.output=std::filesystem::absolute(value);
            else if(argument==L"--capture")config.capture=std::filesystem::absolute(value);
            else if(argument==L"--source-id"){if(value.size()!=64||value.find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring::npos)throw std::invalid_argument("source-id");for(const auto c:value)config.sourceIdentity+=static_cast<char>(c);}
            else if(argument==L"--mode"){if(value!=L"sync"&&value!=L"async")throw std::invalid_argument("mode");config.asynchronous=value==L"async";}
            else if(argument==L"--language"){if(value!=L"en-US"&&value!=L"zh-CN")throw std::invalid_argument("language");language=value==L"zh-CN"?"zh-CN":"en-US";}
            else if(argument==L"--frames")config.frames=number(value);
            else if(argument==L"--trigger-frame")config.triggerFrame=number(value);
            else if(argument==L"--frame-ms")config.frameMilliseconds=number(value);
            else if(argument==L"--budget-bytes")config.budget.bytes=number(value);
            else if(argument==L"--budget-tasks")config.budget.tasks=number(value);
            else if(argument==L"--cancel-frame")config.cancelFrame=number(value);
            else if(argument==L"--restart-frame")config.restartFrame=number(value);
            else throw std::invalid_argument("arguments");
        }
        if(!text.Load(GetExecutableDirectory()/"Resources/Localization"/(language+".txt"),error))throw std::runtime_error("localization");
        if(!prepare.empty())
        {
            std::vector<AssetDiagnostic> diagnostics;if(!CreateLoadingWorkload(prepare,light,diagnostics))throw std::runtime_error(diagnostics.empty()?"workload":diagnostics.back().detail);
            std::cout<<text.GetText("Sample.LoadingComplete")<<'\n';return 0;
        }
        if(config.workload.empty()||config.output.empty()||light)throw std::invalid_argument("configuration");return RunLoadingExperiment(config,text);
    }
    catch(const std::exception& exception){std::cerr<<text.GetText("Sample.LoadingFailed")<<": "<<exception.what()<<'\n';return 2;}
}
