#include "ExternalProcessBroker.h"
#include "EditorProcessRunner.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Log.h"
#include <Windows.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <stdexcept>

namespace se
{
namespace
{
using Json=nlohmann::json;namespace fs=std::filesystem;
constexpr wchar_t directoryKey[]=L"SELFENGINE_EXTERNAL_LAUNCH_DIRECTORY";
constexpr wchar_t tokenKey[]=L"SELFENGINE_EXTERNAL_LAUNCH_TOKEN";
std::wstring EnvironmentValue(const wchar_t* key)
{const auto count=GetEnvironmentVariableW(key,nullptr,0);if(!count||count>32768)return {};std::wstring value(count,L'\0');const auto read=GetEnvironmentVariableW(key,value.data(),count);if(!read||read>=count)return {};value.resize(read);return value;}
// 参数是任意文本，不能借助 filesystem::generic_* 转码，否则会把反斜杠改成路径分隔符。
std::string Utf8(const std::wstring& value)
{
    if(value.empty())return {};const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(!count)throw std::invalid_argument("Editor.ProcessArgumentsInvalid");std::string result(count,'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr);return result;
}
std::wstring Wide(const std::string& value)
{
    if(value.empty())return {};const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(!count)throw std::invalid_argument("Editor.ProcessArgumentsInvalid");std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count);return result;
}
bool SafeDirectory(const fs::path& directory)
{
    if(!directory.is_absolute())return false;
    for(auto path=directory;!path.empty();path=path.parent_path())
    {const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES||(flags&FILE_ATTRIBUTE_REPARSE_POINT))return false;if(path==path.root_path())break;}
    return true;
}
bool WriteJson(const fs::path& path,const Json& value)
{
    const auto temporary=path.wstring()+L".tmp";
    {std::ofstream output(temporary,std::ios::binary|std::ios::trunc);if(!output)return false;output<<value.dump();if(!output)return false;}
    return MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
DWORD StartDirect(const std::wstring& executable,const std::vector<std::wstring>& arguments,const fs::path& directory,bool show,bool association)
{
    if(association)
    {
        SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI|SEE_MASK_NOASYNC;
        info.lpVerb=L"open";info.lpFile=executable.c_str();info.lpDirectory=directory.c_str();info.nShow=SW_SHOWNORMAL;
        if(!ShellExecuteExW(&info))return GetLastError();if(info.hProcess)CloseHandle(info.hProcess);return ERROR_SUCCESS;
    }
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=show?SW_SHOWNORMAL:SW_HIDE;
    PROCESS_INFORMATION process{};auto command=BuildProcessCommandLine(executable,arguments);
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,nullptr,
        directory.c_str(),&startup,&process))return GetLastError();
    LogCheckpoint("Process.External.Start","pid="+std::to_string(process.dwProcessId));
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return ERROR_SUCCESS;
}
}
struct ExternalProcessBroker::Impl{fs::path directory;std::string token;bool ready=false;};
ExternalProcessBroker::ExternalProcessBroker(const fs::path& parent):impl_(std::make_unique<Impl>())
{
    auto& p=*impl_;
    try{p.token=asset_io::NewId();std::error_code error;p.directory=fs::absolute(parent/("external-launch-"+p.token),error);
        if(!error)p.ready=fs::create_directory(p.directory,error)&&!error&&SafeDirectory(p.directory);}
    catch(...){p.ready=false;}
}
ExternalProcessBroker::~ExternalProcessBroker()
{if(impl_->ready&&SafeDirectory(impl_->directory)){std::error_code error;fs::remove_all(impl_->directory,error);}}
bool ExternalProcessBroker::Ready() const{return impl_->ready;}
std::vector<wchar_t> ExternalProcessBroker::Environment() const
{
    struct Less{bool operator()(const std::wstring& a,const std::wstring& b)const{return _wcsicmp(a.c_str(),b.c_str())<0;}};
    std::map<std::wstring,std::wstring,Less> values;
    auto* environment=GetEnvironmentStringsW();if(!environment)return {};
    for(const wchar_t* item=environment;*item;item+=wcslen(item)+1)
    {std::wstring entry(item);const auto separator=entry.find(L'=',entry.front()==L'='?1:0);if(separator!=std::wstring::npos)values[entry.substr(0,separator)]=entry.substr(separator+1);}
    FreeEnvironmentStringsW(environment);
    values[directoryKey]=impl_->directory.wstring();values[tokenKey]=Wide(impl_->token);
    std::vector<wchar_t> block;
    for(const auto& [key,value]:values){const auto entry=key+L"="+value;block.insert(block.end(),entry.begin(),entry.end());block.push_back(L'\0');}block.push_back(L'\0');return block;
}
void ExternalProcessBroker::Pump(bool allowLaunch) noexcept
{
    try
    {
        auto& p=*impl_;if(!p.ready||!SafeDirectory(p.directory))return;
        unsigned count=0;
        for(const auto& entry:fs::directory_iterator(p.directory))
        {
            if(++count>64)break;
            const auto name=entry.path().filename().string();
            if(!name.starts_with("request-")||entry.path().extension()!=L".json"||name.size()>64||!entry.is_regular_file()||entry.file_size()>65536)continue;
            const auto flags=GetFileAttributesW(entry.path().c_str());if(flags==INVALID_FILE_ATTRIBUTES||(flags&FILE_ATTRIBUTE_REPARSE_POINT))continue;
            DWORD result=ERROR_INVALID_DATA;
            try
            {
                std::ifstream input(entry.path(),std::ios::binary);const auto request=Json::parse(input);
                if(request.at("token")==p.token&&request.at("schema")==1&&request.at("expires").get<ULONGLONG>()>=GetTickCount64())
                {
                    auto arguments=request.at("arguments").get<std::vector<std::string>>();
                    std::vector<std::wstring> wide;std::size_t total=0;
                    for(const auto& value:arguments){total+=value.size();wide.push_back(Wide(value));}
                    if(arguments.size()<=64&&total<=32760)
                        result=allowLaunch?StartDirect(Wide(request.at("executable").get<std::string>()),wide,
                            asset_io::FromUtf8(request.at("directory").get<std::string>()),request.value("show",false),request.value("association",false)):ERROR_CANCELLED;
                }
            }
            catch(...){}
            const auto reply=p.directory/("response-"+name.substr(8));WriteJson(reply,{{"schema",1},{"error",result}});
            std::error_code error;fs::remove(entry.path(),error);
        }
    }
    catch(...){LogCheckpoint("Process.External.BrokerError");}
}
bool LaunchExternalApplication(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const fs::path& workingDirectory,bool show,bool association,std::string& error)
{
    error.clear();
    try
    {
        const auto directory=fs::path(EnvironmentValue(directoryKey));const auto token=Utf8(EnvironmentValue(tokenKey));
        const auto working=workingDirectory.empty()?fs::current_path():workingDirectory;
        if(directory.empty())
        {
            BOOL inJob=FALSE;
            if(!IsProcessInJob(GetCurrentProcess(),nullptr,&inJob)||inJob){error="Editor.ExternalLaunchBrokerUnavailable";return false;}
            const auto result=StartDirect(executable,arguments,working,show,association);
            if(result){LogCheckpoint("Process.External.Failed","win32="+std::to_string(result));error="Editor.ProcessStartFailed";return false;}return true;
        }
        if(token.empty()||!SafeDirectory(directory)){error="Editor.ExternalLaunchBrokerUnavailable";return false;}
        const auto id=asset_io::NewId();const auto request=directory/("request-"+id+".json"),response=directory/("response-"+id+".json");
        std::vector<std::string> utf8;for(const auto& value:arguments)utf8.push_back(Utf8(value));
        if(!WriteJson(request,{{"schema",1},{"token",token},{"expires",GetTickCount64()+5000},{"executable",Utf8(executable)},
            {"arguments",utf8},{"directory",asset_io::Utf8(working)},{"show",show},{"association",association}}))
        {error="Editor.ExternalLaunchBrokerUnavailable";return false;}
        const auto deadline=GetTickCount64()+5000;
        while(GetTickCount64()<deadline)
        {
            std::error_code ignored;
            if(fs::is_regular_file(response,ignored))
            {
                std::ifstream input(response,std::ios::binary);const auto reply=Json::parse(input);input.close();fs::remove(response,ignored);
                const auto result=reply.at("error").get<DWORD>();if(!result)return true;
                LogCheckpoint("Process.External.Failed","win32="+std::to_string(result));error="Editor.ProcessStartFailed";return false;
            }
            Sleep(10);
        }
        std::error_code ignored;fs::remove(request,ignored);error="Editor.ExternalLaunchBrokerUnavailable";return false;
    }
    catch(...){error="Editor.ExternalLaunchBrokerUnavailable";return false;}
}
}
