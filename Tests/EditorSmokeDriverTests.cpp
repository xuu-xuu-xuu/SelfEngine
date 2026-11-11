#include "ToolLayer/Editor/EditorSmokeDriver.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct EnvironmentGuard
{
    std::vector<std::pair<std::wstring,std::wstring>> values;
    EnvironmentGuard()
    {
        for(const auto name:{L"SELFENGINE_EDITOR_SMOKE_REPORT",L"SELFENGINE_EDITOR_SMOKE_EXPORT",L"SELFENGINE_EDITOR_SMOKE_PLAYBACK",L"SELFENGINE_EDITOR_SMOKE_CANCEL_MS",L"SELFENGINE_EDITOR_MANAGED_HOST",L"SELFENGINE_EDITOR_SMOKE_WORKFLOW",L"SELFENGINE_EDITOR_SMOKE_WORKFLOW_STAGE",L"SELFENGINE_EDITOR_SMOKE_HOST_START",L"SELFENGINE_EDITOR_SMOKE_SUPERVISOR_PID",L"SELFENGINE_EDITOR_SMOKE_SUPERVISOR_START"})
        {
            const auto length=GetEnvironmentVariableW(name,nullptr,0);std::wstring value(length,L'\0');
            if(length)value.resize(GetEnvironmentVariableW(name,value.data(),length));
            values.emplace_back(name,std::move(value));SetEnvironmentVariableW(name,nullptr);
        }
    }
    ~EnvironmentGuard(){for(const auto& [name,value]:values)SetEnvironmentVariableW(name.c_str(),value.empty()?nullptr:value.c_str());}
};
nlohmann::json Read(const std::filesystem::path& path){std::ifstream input(path);return nlohmann::json::parse(input);}
}
int wmain()
{
    try
    {
        EnvironmentGuard environment;
        const auto directory=std::filesystem::absolute(std::filesystem::path(".cache/validation/editor-smoke-driver")/(std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())));
        std::filesystem::create_directories(directory);
        auto disabled=se::EditorSmokeDriver::FromEnvironment(directory);
        Check(!disabled.Enabled()&&disabled.FinalResult(37)==37,"disabled driver preserves host exit exactly");
        const auto report=directory/"incomplete.json";
        SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_REPORT",report.c_str());SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_PLAYBACK",L"1");
        auto incomplete=se::EditorSmokeDriver::FromEnvironment(directory);
        Check(incomplete.Enabled()&&incomplete.FinalResult(0)==1,"host close before command execution cannot pass a smoke test");
        auto value=Read(report);
        Check(value["success"]==false&&value["failure_check"]=="host_ended_before_smoke"&&value["host_exit_code"]==0&&value["manual_input_verified"]==false,"incomplete report distinguishes host exit from smoke result and manual evidence");
        const auto failed=directory/"host-failed.json";
        SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_REPORT",failed.c_str());SetEnvironmentVariableW(L"SELFENGINE_EDITOR_MANAGED_HOST",L"1");
        auto failedHost=se::EditorSmokeDriver::FromEnvironment(directory);
        Check(failedHost.FinalResult(37)==37,"driver does not replace the host failure code with generic smoke failure");
        value=Read(failed);
        Check(value["success"]==false&&value["host_exit_code"]==37&&value["managed_host"]==true,"host failure and managed route are recorded");
        SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_REPORT",nullptr);
        auto missingReport=se::EditorSmokeDriver::FromEnvironment(directory);
        Check(missingReport.Enabled()&&missingReport.FinalResult(0)!=0,"an enabled smoke without a report path cannot claim completion");
        const auto workflowDirectory=directory/"out/Editor/workflow-smoke/unit";std::filesystem::create_directories(workflowDirectory);
        std::filesystem::create_directories(directory/"Scripts");{std::ofstream source(directory/"Scripts/Spin.cs");source<<"// validation fixture\n";}
        const auto workflowFile=workflowDirectory/"workflow.json";
        const auto utf8=directory.generic_u8string();const std::string rootText(reinterpret_cast<const char*>(utf8.data()),utf8.size());
        nlohmann::json workflow{{"schemaVersion",1},{"kind","SelfEngine.EditorWorkflowSmoke"},{"projectRoot",rootText},
            {"stages",nlohmann::json::array({{{"mode","playback"},{"expectedBuild","ready"},{"playCycles",1},{"radiansPerSecond",1.0}}})}};
        const auto write=[&]{std::ofstream output(workflowFile);output<<workflow.dump();};
        std::size_t count=0;std::string detail;write();
        Check(se::EditorSmokeDriver::ValidateWorkflow(directory,workflowFile,count,detail)&&count==1,"isolated finite workflow accepted");
        workflow["stages"][0]["playCycles"]=21;write();
        Check(!se::EditorSmokeDriver::ValidateWorkflow(directory,workflowFile,count,detail),"more than twenty same-process play cycles rejected");
        workflow["stages"][0]["playCycles"]=1;
        const auto stage=workflow["stages"][0];workflow["stages"]=nlohmann::json::array();
        for(int index=0;index<22;++index)workflow["stages"].push_back(stage);write();
        Check(!se::EditorSmokeDriver::ValidateWorkflow(directory,workflowFile,count,detail),"unbounded supervisor restart list rejected");
        workflow["stages"]=nlohmann::json::array({stage,stage});workflow["stages"][0]["patchAfter"]="../outside.txt";workflow["stages"][0]["sceneEditName"]="saved";write();
        Check(!se::EditorSmokeDriver::ValidateWorkflow(directory,workflowFile,count,detail),"source replacement escape rejected");
        workflow["stages"]=nlohmann::json::array({stage});write();
        const auto outside=directory/"workflow.json";std::filesystem::copy_file(workflowFile,outside);
        Check(!se::EditorSmokeDriver::ValidateWorkflow(directory,outside,count,detail),"configuration outside dedicated fixture directory rejected");
        std::cout<<"EditorSmokeDriverTests passed (result contracts and bounded isolated workflow validation).\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"EditorSmokeDriverTests failed: "<<exception.what()<<'\n';return 1;}
}
