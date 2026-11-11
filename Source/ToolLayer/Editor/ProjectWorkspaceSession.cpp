#include "ProjectWorkspaceSession.h"
#include "ToolLayer/Tools/Project.h"
#include "CoreLayer/Hash.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <iostream>

namespace se
{
namespace
{
using Json=nlohmann::json;
constexpr size_t MaximumRecentProjects=20;
constexpr size_t MaximumManifestBytes=1024*1024;

std::string Utf8(const std::filesystem::path& path)
{
    const auto value=path.wstring();
    if(value.empty())return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(size<=0)return {};
    std::string result(static_cast<size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr);
    return result;
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
std::string NowUtc()
{
    const auto now=std::chrono::system_clock::now();
    const std::time_t time=std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc,&time);
    char buffer[32]{};
    std::strftime(buffer,sizeof(buffer),"%Y-%m-%dT%H:%M:%SZ",&utc);
    return buffer;
}
std::string IdentityHash()
{
    wchar_t user[256]{},domain[256]{};
    DWORD userSize=256,domainSize=256;
    std::string identity="unknown";
    if(GetUserNameW(user,&userSize))
    {
        if(GetEnvironmentVariableW(L"USERDOMAIN",domain,domainSize)>0)identity=Utf8(std::filesystem::path(domain))+"/";
        identity+=Utf8(std::filesystem::path(user));
    }
    const auto digest=Sha256(identity);
    return digest.empty()?"unknown":digest.substr(0,16);
}
bool WriteAtomic(const std::filesystem::path& path,const std::string& text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(),error);
    if(error)return false;
    static std::atomic<unsigned long> sequence=0;
    const auto temporary=path.parent_path()/(path.filename().string()+".tmp-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(sequence.fetch_add(1)));
    {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        if(!output)return false;
        output.write(text.data(),static_cast<std::streamsize>(text.size()));
        if(!output)return false;
    }
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temporary,error);return false;
    }
    return true;
}
enum class PreferenceState { Missing,Valid,Corrupt,Unsupported };
PreferenceState ReadPreferenceFile(const std::filesystem::path& path,Json& json,std::string& raw)
{
    std::error_code error;
    if(!std::filesystem::exists(path,error))return error?PreferenceState::Corrupt:PreferenceState::Missing;
    try
    {
        if(!std::filesystem::is_regular_file(path,error)||std::filesystem::file_size(path,error)>MaximumManifestBytes||error)
            return PreferenceState::Corrupt;
        std::ifstream input(path,std::ios::binary);
        if(!input)return PreferenceState::Corrupt;
        raw=std::string{std::istreambuf_iterator<char>(input),{}};
        json=Json::parse(raw);
        if(!json.is_object()||!json.contains("schema")||!json["schema"].is_number_unsigned())return PreferenceState::Corrupt;
        if(json["schema"]!=1)return PreferenceState::Unsupported;
        const auto language=json.value("language",std::string("zh-CN"));
        const auto scale=json.value("ui_scale",1.0f);
        (void)json.value("external_editor",std::string{});
        (void)json.value("restore_last_project",true);
        (void)json.value("last_project",std::string{});
        if(language.empty()||!std::isfinite(scale)||scale<=0||scale>8)return PreferenceState::Corrupt;
        auto recent=json.value("recent",Json::array());
        if(!recent.is_array())return PreferenceState::Corrupt;
        for(auto& item:recent)
        {
            if(item.is_string())item=Json{{"name",""},{"path",item.get<std::string>()},{"last_opened",""}};
            if(!item.is_object())return PreferenceState::Corrupt;
            (void)item.value("name",std::string{});
            (void)item.value("path",std::string{});
            (void)item.value("last_opened",std::string{});
        }
        json["recent"]=std::move(recent);
        return PreferenceState::Valid;
    }
    catch(const std::exception&){return PreferenceState::Corrupt;}
}
bool ReadPreferences(const std::filesystem::path& directory,Json& json,std::string& error)
{
    error.clear();std::string raw;
    const auto state=ReadPreferenceFile(directory/"preferences.json",json,raw);
    if(state==PreferenceState::Valid)return true;
    if(state==PreferenceState::Unsupported){error="Editor.StorageUnsupportedSchema";return false;}
    Json backup;const auto backupState=ReadPreferenceFile(directory/"preferences.json.bak",backup,raw);
    if(backupState==PreferenceState::Valid){json=std::move(backup);error="Editor.StorageBackupUsed";return true;}
    if(state==PreferenceState::Missing&&backupState==PreferenceState::Missing)
    {json={{"schema",1},{"recent",Json::array()}};return true;}
    error=backupState==PreferenceState::Unsupported?"Editor.StorageUnsupportedSchema":"Editor.StorageCorrupt";
    return false;
}
template<class Update> bool UpdatePreferences(const std::filesystem::path& directory,Update update,std::string& error)
{
    error.clear();
    if(directory.empty()){error="Editor.StorageInvalidValue";return false;}
    std::error_code directoryError;std::filesystem::create_directories(directory,directoryError);
    if(directoryError){error="Editor.StorageWriteFailed";return false;}
    const auto lockPath=directory/"preferences.lock";
    HANDLE lock=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock==INVALID_HANDLE_VALUE){error="Editor.StorageWriteFailed";return false;}
    struct LockGuard{HANDLE handle;~LockGuard(){CloseHandle(handle);}} lockGuard{lock};
    const auto path=directory/"preferences.json";
    Json json;std::string raw;
    const auto state=ReadPreferenceFile(path,json,raw);
    if(state==PreferenceState::Unsupported){error="Editor.StorageUnsupportedSchema";return false;}
    if(state!=PreferenceState::Valid)
    {
        Json backup;std::string backupRaw;
        const auto backupState=ReadPreferenceFile(directory/"preferences.json.bak",backup,backupRaw);
        if(backupState==PreferenceState::Unsupported){error="Editor.StorageUnsupportedSchema";return false;}
        json=backupState==PreferenceState::Valid?std::move(backup):Json{{"schema",1},{"recent",Json::array()}};
    }
    update(json);
    // 只备份已经通过结构检查的主文件；坏原文单独留档，不污染有效备份。
    if(state==PreferenceState::Valid&&!WriteAtomic(directory/"preferences.json.bak",raw))
    {error="Editor.StorageWriteFailed";return false;}
    if(state==PreferenceState::Corrupt)
    {
        const auto archived=directory/("preferences.json.corrupt-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        std::error_code copyError;
        if(!std::filesystem::copy_file(path,archived,std::filesystem::copy_options::none,copyError))
        {error="Editor.StorageWriteFailed";return false;}
    }
    if(!WriteAtomic(path,json.dump(2)+"\n")){error="Editor.StorageWriteFailed";return false;}
    return true;
}
std::string ProjectIdentity(const std::filesystem::path& manifest)
{
    return Sha256(Utf8(manifest)).substr(0,32);
}
bool ReservedName(const std::wstring& name)
{
    std::wstring stem=name;
    const auto dot=stem.find(L'.');
    if(dot!=std::wstring::npos)stem=stem.substr(0,dot);
    std::transform(stem.begin(),stem.end(),stem.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    static const wchar_t* reserved[]={L"con",L"prn",L"aux",L"nul"};
    for(const auto* value:reserved)if(stem==value)return true;
    if(stem.size()==4&&(stem.rfind(L"com",0)==0||stem.rfind(L"lpt",0)==0)&&stem[3]>=L'1'&&stem[3]<=L'9')return true;
    return false;
}
}

const char* ProjectTemplateId(ProjectTemplate value)
{
    return value==ProjectTemplate::BlankScene?"blank-scene":"basic-3d";
}
const char* ProjectTemplateNameKey(ProjectTemplate value)
{
    return value==ProjectTemplate::BlankScene?"Editor.TemplateBlank":"Editor.TemplateBasic3D";
}

ProjectLease::~ProjectLease(){Release();}
ProjectLease::ProjectLease(ProjectLease&& other) noexcept:handle_(other.handle_){other.handle_=nullptr;}
ProjectLease& ProjectLease::operator=(ProjectLease&& other) noexcept
{
    if(this!=&other){Release();handle_=other.handle_;other.handle_=nullptr;}
    return *this;
}
void ProjectLease::Release() noexcept
{
    if(handle_){CloseHandle(static_cast<HANDLE>(handle_));handle_=nullptr;}
}
ProjectLease ProjectLease::Acquire(const std::filesystem::path& userDirectory,const std::filesystem::path& manifest,
    bool& acquired,std::string& occupant)
{
    acquired=false;occupant.clear();
    ProjectLease lease;
    std::error_code error;
    const auto directory=userDirectory/"leases";
    std::filesystem::create_directories(directory,error);
    if(error)return lease;
    std::error_code canonicalError;
    const auto canonicalManifest=std::filesystem::weakly_canonical(manifest,canonicalError);
    const auto identity=canonicalError?manifest:canonicalManifest;
    const auto path=directory/(ProjectIdentity(identity)+".lock");
    HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)
    {
        if(GetLastError()!=ERROR_SHARING_VIOLATION)return lease;
        HANDLE reader=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(reader!=INVALID_HANDLE_VALUE)
        {
            char buffer[1024]{};DWORD bytes=0;
            if(ReadFile(reader,buffer,sizeof(buffer)-1,&bytes,nullptr)&&bytes)occupant.assign(buffer,bytes);
            CloseHandle(reader);
        }
        return lease;
    }
    const Json owner{{"pid",GetCurrentProcessId()},{"created",NowUtc()},{"manifest",Utf8(identity)}};
    const auto text=owner.dump(2)+"\n";
    DWORD written=0;
    SetFilePointer(handle,0,nullptr,FILE_BEGIN);SetEndOfFile(handle);
    WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr);
    FlushFileBuffers(handle);
    lease.handle_=handle;acquired=true;return lease;
}

std::filesystem::path ProjectWorkspaceSession::EngineRootFromExecutable()
{
    std::error_code error;
    wchar_t configured[32768]{};
    const auto length=GetEnvironmentVariableW(L"SELFENGINE_EDITOR_ENGINE_ROOT",configured,32768);
    if(length>0&&length<32768)
    {
        const std::filesystem::path root(configured);
        if(root.is_absolute()&&std::filesystem::is_directory(root/"Resources",error))return root;
    }
    const auto directory=GetExecutableDirectory();
    if(std::filesystem::is_regular_file(directory/"editor-package.json",error))return directory;
    for(auto candidate=directory;!candidate.empty();candidate=candidate.parent_path())
    {
        if(std::filesystem::exists(candidate/"CMakeLists.txt",error)&&std::filesystem::exists(candidate/"Resources/Localization/zh-CN.txt",error))return candidate;
        if(candidate==candidate.root_path())break;
    }
    return directory;
}
std::filesystem::path ProjectWorkspaceSession::UserDataDirectory(const std::filesystem::path& engineRoot)
{
    std::error_code error;
    const bool development=std::filesystem::exists(engineRoot/"CMakeLists.txt",error)
        &&std::filesystem::exists(engineRoot/"Resources/Localization/zh-CN.txt",error);
    const auto localAppDataPath=[]() -> std::filesystem::path {
        wchar_t localAppData[MAX_PATH]{};
        const auto length=GetEnvironmentVariableW(L"LOCALAPPDATA",localAppData,MAX_PATH);
        if(length>0&&length<MAX_PATH)return std::filesystem::path(localAppData)/"SelfEngine"/"Editor"/IdentityHash();
        return {};
    };
    if(development)
    {
        // 开发构建优先使用 out/（设计约定）；若构建目录 ACL 只读，则退回工作区内被忽略的 .cache，
        // 避免项目租约/布局在启动阶段因写入失败直接退出。
        const std::filesystem::path candidates[] = {
            engineRoot/"out"/"Editor"/"User"/IdentityHash(),
            engineRoot/".cache"/"Editor"/"User"/IdentityHash(),
        };
        for(const auto& candidate:candidates)
        {
            std::error_code createError;
            std::filesystem::create_directories(candidate,createError);
            if(createError)continue;
            const auto probe=candidate/"lease-write-probe.tmp";
            std::ofstream output(probe,std::ios::binary|std::ios::trunc);
            if(!output)continue;
            output.close();
            std::filesystem::remove(probe,createError);
            return candidate;
        }
    }
    const auto local=localAppDataPath();
    return local.empty()?engineRoot/"out"/"Editor"/"User"/IdentityHash():local;
}

ProjectPreflight ProjectWorkspaceSession::Preflight(const std::filesystem::path& requested)
{
    ProjectPreflight result;
    try
    {
        auto path=requested;
        if(path.is_relative())path=std::filesystem::current_path()/path;
        std::error_code error;
        path=std::filesystem::weakly_canonical(path,error);
        if(error||path.empty()){result.errorKey="Project.ManifestNotFound";result.detail="path cannot be resolved";return result;}
        if(std::filesystem::is_directory(path,error))path/=L"project.json";
        if(path.filename()!=L"project.json"){result.errorKey="Project.ManifestPath";result.detail="expected project.json";return result;}
        if(!std::filesystem::is_regular_file(path,error)){result.errorKey="Project.ManifestNotFound";result.detail=Utf8(path);return result;}
        if(std::filesystem::file_size(path,error)>MaximumManifestBytes){result.errorKey="Project.InvalidManifest";result.detail="manifest too large";return result;}
        std::ifstream input(path,std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(input),{}};
        const auto json=Json::parse(text);
        if(json.at("format")!="SelfEngine.Project"){result.errorKey="Project.InvalidManifest";result.detail="format";return result;}
        if(!json.at("version").is_number_integer()||json.at("version")!=1){result.errorKey="Project.UnsupportedVersion";result.detail=json.at("version").dump();return result;}
        if(json.at("name").get<std::string>().empty()){result.errorKey="Project.InvalidManifest";result.detail="name";return result;}
        const auto contentRoot=json.at("contentRoot").get<std::string>();
        const auto entryScene=json.at("entryScene").get<std::string>();
        if(contentRoot.empty()||entryScene.empty()){result.errorKey="Project.InvalidManifest";result.detail="contentRoot or entryScene";return result;}
        result.root=path.parent_path();result.manifest=path;
        result.contentRoot=std::filesystem::weakly_canonical(result.root/Wide(contentRoot),error);
        if(error||!std::filesystem::is_directory(result.contentRoot,error)){result.errorKey="Project.MissingContentRoot";result.detail=contentRoot;return result;}
        result.entryScene=std::filesystem::weakly_canonical(result.root/Wide(entryScene),error);
        if(error||!std::filesystem::is_regular_file(result.entryScene,error)){result.errorKey="Project.MissingEntryScene";result.detail=entryScene;return result;}
        const auto probe=result.root/("."+ProjectIdentity(path)+".probe");
        {std::ofstream probeOutput(probe,std::ios::binary|std::ios::trunc);if(probeOutput)probeOutput.put('\0');}
        result.writable=std::filesystem::exists(probe,error);
        std::filesystem::remove(probe,error);
        result.ok=true;return result;
    }
    catch(const std::exception& failure)
    {
        result.errorKey="Project.InvalidManifest";result.detail=failure.what();return result;
    }
}

bool ProjectWorkspaceSession::ValidateProjectName(const std::string& name,std::string& detail)
{
    detail.clear();
    const auto wide=Wide(name);
    if(wide.empty()){detail="empty";return false;}
    if(wide.size()>128){detail="too long";return false;}
    if(name=="."||name==".."){detail="relative name";return false;}
    if(name.find_first_of("<>:\"/\\|?*")!=std::string::npos){detail="reserved character";return false;}
    if(name.back()=='.'||name.back()==' '){detail="trailing dot or space";return false;}
    for(const auto character:name)if(static_cast<unsigned char>(character)<0x20){detail="control character";return false;}
    if(ReservedName(wide)){detail="reserved device name";return false;}
    return true;
}

bool ProjectWorkspaceSession::CreateProject(ProjectTemplate projectTemplate,const std::string& name,const std::filesystem::path& parent,
    const std::filesystem::path& templateRoot,ProjectPreflight& output,std::string& errorKey,std::string& detail)
{
    errorKey.clear();detail.clear();
    if(!ValidateProjectName(name,detail)){errorKey="Project.InvalidName";return false;}
    std::error_code error;
    const auto base=std::filesystem::weakly_canonical(parent,error);
    if(error||!std::filesystem::is_directory(base,error)){errorKey="Project.ParentMissing";detail=Utf8(parent);return false;}
    const auto target=base/Wide(name);
    if(std::filesystem::exists(target,error))
    {
        if(!std::filesystem::is_directory(target,error)||!std::filesystem::is_empty(target,error))
        {errorKey="Project.TargetNotEmpty";detail=Utf8(target);return false;}
    }
    const auto templateDirectory=templateRoot/(projectTemplate==ProjectTemplate::BlankScene?L"BlankScene":L"EmptyGame");
    if(!std::filesystem::is_regular_file(templateDirectory/L"project.json",error))
    {errorKey="Project.TemplateMissing";detail=Utf8(templateDirectory);return false;}
    const auto candidate=base/(std::wstring(L".")+Wide(name)+L".candidate-"+Wide(std::to_string(GetTickCount64())));
    std::filesystem::remove_all(candidate,error);
    std::vector<ProjectDiagnostic> diagnostics;
    if(!::se::CreateProject(templateDirectory,candidate,diagnostics))
    {
        errorKey=diagnostics.empty()?"Project.CreateFailed":diagnostics.back().key;
        detail=diagnostics.empty()?Utf8(candidate):diagnostics.back().detail;
        std::filesystem::remove_all(candidate,error);return false;
    }
    const auto preflight=Preflight(candidate);
    if(!preflight.ok)
    {
        errorKey=preflight.errorKey;detail=preflight.detail;
        std::filesystem::remove_all(candidate,error);return false;
    }
    if(std::filesystem::exists(target,error))
    {
        if(!std::filesystem::is_empty(target,error)||!std::filesystem::remove(target,error))
        {errorKey="Project.TargetNotEmpty";detail=Utf8(target);std::filesystem::remove_all(candidate,error);return false;}
    }
    std::filesystem::rename(candidate,target,error);
    if(error)
    {
        errorKey="Project.CreateFailed";detail=Utf8(target);
        std::filesystem::remove_all(candidate,error);return false;
    }
    output=Preflight(target);
    if(!output.ok){errorKey=output.errorKey;detail=output.detail;return false;}
    return true;
}

bool ProjectWorkspaceSession::Load(const std::filesystem::path& userDirectory,std::string& error)
{
    error.clear();directory_=userDirectory;recent_.clear();language_="zh-CN";restoreLast_=true;lastProject_.clear();
    languageChanged_=restoreChanged_=false;
    Json json;
    if(!ReadPreferences(directory_,json,error))return false;
    std::error_code fileError;
    try
    {
        language_=json.value("language",std::string("zh-CN"));
        restoreLast_=json.value("restore_last_project",true);
        lastProject_=json.value("last_project",std::string{});
        for(const auto& item:json.value("recent",Json::array()))
        {
            if(recent_.size()>=MaximumRecentProjects)break;
            RecentProject project;
            project.name=item.value("name",std::string{});
            project.path=item.value("path",std::string{});
            project.lastOpenedUtc=item.value("last_opened",std::string{});
            if(project.path.empty())continue;
            project.available=std::filesystem::is_regular_file(std::filesystem::path(Wide(project.path)),fileError);
            recent_.push_back(std::move(project));
        }
        return true;
    }
    catch(const std::exception& failure)
    {
        (void)failure;error="Editor.StorageCorrupt";recent_.clear();return false;
    }
}
bool ProjectWorkspaceSession::Save(std::string& error) const
{
    if(language_.empty()){error="Editor.StorageInvalidValue";return false;}
    const bool saved=UpdatePreferences(directory_,[&](Json& document)
    {
        Json recent=Json::array();
        for(const auto& project:recent_)
        {
            Json item=Json::object();
            for(const auto& old:document.value("recent",Json::array()))
                if(old.value("path",std::string{})==project.path){item=old;break;}
            item["name"]=project.name;item["path"]=project.path;item["last_opened"]=project.lastOpenedUtc;
            recent.push_back(std::move(item));
        }
        document["schema"]=1;
        if(languageChanged_||!document.contains("language"))document["language"]=language_;
        if(restoreChanged_||!document.contains("restore_last_project"))document["restore_last_project"]=restoreLast_;
        document["last_project"]=lastProject_;document["recent"]=std::move(recent);
    },error);
    if(saved)languageChanged_=restoreChanged_=false;
    return saved;
}
bool ProjectWorkspaceSession::LoadPreferences(const std::filesystem::path& directory,EditorPreferences& output,std::string& error)
{
    Json json;if(!ReadPreferences(directory,json,error))return false;
    EditorPreferences preferences;
    preferences.language=json.value("language",std::string("zh-CN"));
    preferences.uiScale=json.value("ui_scale",1.0f);
    preferences.externalEditor=json.value("external_editor",std::string{});
    preferences.colorLut=json.value("color_lut",std::string{});
    preferences.restoreLastProject=json.value("restore_last_project",true);
    for(const auto& item:json.value("recent",Json::array()))
    {
        const auto path=item.value("path",std::string{});
        if(!path.empty())preferences.recentProjects.push_back(path);
    }
    output=std::move(preferences);return true;
}
bool ProjectWorkspaceSession::SavePreferences(const std::filesystem::path& directory,const EditorPreferences& preferences,std::string& error)
{
    if(preferences.language.empty()||!std::isfinite(preferences.uiScale)||preferences.uiScale<=0||preferences.uiScale>8)
    {error="Editor.StorageInvalidValue";return false;}
    std::error_code fileError;
    const bool initializeRecent=!std::filesystem::exists(directory/"preferences.json",fileError)
        &&!std::filesystem::exists(directory/"preferences.json.bak",fileError);
    return UpdatePreferences(directory,[&](Json& document)
    {
        document["schema"]=1;document["language"]=preferences.language;document["ui_scale"]=preferences.uiScale;
        document["external_editor"]=preferences.externalEditor;document["restore_last_project"]=preferences.restoreLastProject;
        document["color_lut"]=preferences.colorLut;
        // 最近项目由工作区拥有；偏好页面的旧快照不能删掉新打开的项目或其元数据。
        if(initializeRecent&&document.value("recent",Json::array()).empty())
            for(const auto& path:preferences.recentProjects)
                if(!path.empty())document["recent"].push_back({{"name",""},{"path",path},{"last_opened",""}});
    },error);
}
bool ProjectWorkspaceSession::RecordOpened(const ProjectPreflight& project)
{
    if(!project.ok)return false;
    const auto path=Utf8(project.manifest);
    recent_.erase(std::remove_if(recent_.begin(),recent_.end(),[&](const auto& item){return item.path==path;}),recent_.end());
    RecentProject entry;
    entry.name=project.root.filename().empty()?Utf8(project.root):Utf8(project.root.filename());
    entry.path=path;entry.lastOpenedUtc=NowUtc();entry.available=true;
    recent_.insert(recent_.begin(),std::move(entry));
    if(recent_.size()>MaximumRecentProjects)recent_.resize(MaximumRecentProjects);
    lastProject_=path;
    return true;
}
bool ProjectWorkspaceSession::RemoveRecent(const std::string& canonicalPath)
{
    const auto before=recent_.size();
    recent_.erase(std::remove_if(recent_.begin(),recent_.end(),[&](const auto& item){return item.path==canonicalPath;}),recent_.end());
    return recent_.size()!=before;
}
}
