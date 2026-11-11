#include "EditorSessionStore.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace se
{
namespace
{
using Json=nlohmann::json;
namespace fs=std::filesystem;
constexpr std::size_t Schema=1,MaximumRecoveryRevisions=5;
constexpr std::uint64_t MaximumManifestBytes=1024*1024;
std::string Utf8(const fs::path& path)
{const auto text=path.generic_u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
fs::path FromUtf8(const std::string& text)
{return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));}
std::string Token()
{
    static std::atomic<unsigned long> sequence=0;
    FILETIME time{};GetSystemTimeAsFileTime(&time);
    const auto ticks=(static_cast<std::uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    std::ostringstream output;
    output<<std::setfill('0')<<std::setw(20)<<ticks<<'-'<<GetCurrentProcessId()<<'-'<<std::setw(10)<<sequence.fetch_add(1);
    return output.str();
}
std::string UtcNow()
{
    SYSTEMTIME time{};GetSystemTime(&time);char result[32]{};
    sprintf_s(result,"%04u-%02u-%02uT%02u:%02u:%02uZ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
    return result;
}
bool ReadBytes(const fs::path& path,std::string& output,std::uint64_t limit=(std::numeric_limits<std::uint32_t>::max)())
{
    std::error_code error;
    if(!fs::is_regular_file(path,error)||error)return false;
    const auto size=fs::file_size(path,error);
    if(error||size>limit)return false;
    std::ifstream input(path,std::ios::binary);if(!input)return false;
    output=std::string{std::istreambuf_iterator<char>(input),{}};
    return !input.bad()&&output.size()==size;
}
bool WriteFileBytes(const fs::path& path,const std::string& text)
{
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    bool ok=true;std::size_t offset=0;
    while(offset<text.size())
    {
        const auto count=static_cast<DWORD>((std::min)(text.size()-offset,static_cast<std::size_t>(1024*1024)));
        DWORD written=0;
        if(!WriteFile(file,text.data()+offset,count,&written,nullptr)||written!=count){ok=false;break;}
        offset+=written;
    }
    if(ok)ok=FlushFileBuffers(file)!=FALSE;
    CloseHandle(file);return ok;
}
bool WriteAtomic(const fs::path& path,const std::string& text)
{
    std::error_code error;fs::create_directories(path.parent_path(),error);if(error)return false;
    const auto temporary=path.parent_path()/(path.filename().wstring()+L".tmp-"+FromUtf8(Token()).wstring());
    const bool written=WriteFileBytes(temporary,text);
    if(written&&MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
    fs::remove(temporary,error);return false;
}
bool SafeToken(const std::string& text)
{
    return !text.empty()&&text.size()<=128&&text!="."&&text!=".."&&text.front()!='.'
        &&std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.';});
}
bool GeneratedCandidateName(std::string_view name)
{
    constexpr std::string_view prefix=".candidate-";
    if(!name.starts_with(prefix))return false;
    const auto token=name.substr(prefix.size());
    if(token.size()<33||token.size()>42||token[20]!='-')return false;
    const auto separator=token.find('-',21);
    if(separator==std::string_view::npos||separator==21||separator>31||token.size()-separator-1!=10)return false;
    const auto time=token.substr(0,20),pid=token.substr(21,separator-21),sequence=token.substr(separator+1);
    const auto decimal=[](std::string_view text){return std::all_of(text.begin(),text.end(),[](char c){return c>='0'&&c<='9';});};
    if(!decimal(time)||!decimal(pid)||!decimal(sequence)||pid.front()=='0')return false;
    std::uint64_t ticks=0;std::uint32_t process=0,serial=0;
    const auto a=std::from_chars(time.data(),time.data()+time.size(),ticks);
    const auto b=std::from_chars(pid.data(),pid.data()+pid.size(),process);
    const auto c=std::from_chars(sequence.data(),sequence.data()+sequence.size(),serial);
    return a.ec==std::errc{}&&b.ec==std::errc{}&&c.ec==std::errc{}&&ticks!=0&&process!=0;
}
bool Digest(const std::string& text)
{return text.size()==64&&std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool MetadataText(const std::string& text)
{
    return !text.empty()&&text.size()<=512&&std::none_of(text.begin(),text.end(),[](unsigned char c){return c<32||c==127;})
        &&MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)>0;
}
fs::path Canonical(const fs::path& path)
{
    std::error_code error;if(path.empty())return {};
    auto result=fs::weakly_canonical(fs::absolute(path,error),error);
    return error?fs::path{}:result;
}
bool SameComponent(const fs::path& a,const fs::path& b)
{
    auto left=a.wstring(),right=b.wstring();
    std::transform(left.begin(),left.end(),left.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    std::transform(right.begin(),right.end(),right.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    return left==right;
}
bool Inside(const fs::path& root,const fs::path& path)
{
    if(root.empty()||path.empty())return false;
    auto parent=root.begin(),child=path.begin();
    for(;parent!=root.end();++parent,++child)if(child==path.end()||!SameComponent(*parent,*child))return false;
    return true;
}
bool ReparsePoint(const fs::path& path)
{
    const auto attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
}
bool RecoveryRoot(const fs::path& directory,fs::path& root)
{
    const auto base=Canonical(directory);if(base.empty())return false;
    root=base/"recovery";
    return !ReparsePoint(root)&&Inside(base,Canonical(root));
}
bool SafeRelativePath(const fs::path& project,const std::string& text)
{
    if(text.empty())return true;
    if(text.size()>32768||MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)<=0)return false;
    if(text.find_first_of(":<>\"|?*")!=std::string::npos||std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32;}))return false;
    const auto path=FromUtf8(text);
    if(path.is_absolute()||path.has_root_path())return false;
    for(const auto& component:path)
        if(component==".."||component=="."||component.empty()||component.native().back()==L'.'||component.native().back()==L' ')return false;
    const auto target=Canonical(project/path);
    return target!=project&&Inside(project,target);
}
std::string ProjectIdentity(const fs::path& project)
{
    auto normalized=project.wstring();
    std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    return Sha256(Utf8(fs::path(normalized)));
}
bool ValidateDocument(const fs::path& project,const RecoveryDocument& document)
{
    return MetadataText(document.documentId)&&MetadataText(document.kind)&&SafeRelativePath(project,document.relativePath)
        &&(document.sourceDigest.empty()||Digest(document.sourceDigest));
}
bool DirectoryBytes(const fs::path& root,std::uint64_t& bytes)
{
    bytes=0;std::error_code error;
    if(!fs::exists(root,error))return !error;
    if(ReparsePoint(root))return false;
    for(fs::recursive_directory_iterator it(root,error),end;it!=end&&!error;it.increment(error))
    {
        if(ReparsePoint(it->path()))return false;
        if(it->is_regular_file(error))
        {
            const auto size=it->file_size(error);
            if(error||size>(std::numeric_limits<std::uint64_t>::max)()-bytes)return false;
            bytes+=size;
        }
    }
    return !error;
}
bool RemoveGroupDirectory(const fs::path& recovery,const fs::path& target)
{
    const auto root=Canonical(recovery),resolved=Canonical(target);
    if(root.empty()||resolved==root||!Inside(root,resolved)||ReparsePoint(target))return false;
    std::uint64_t unused=0;if(!DirectoryBytes(target,unused))return false;
    std::error_code error;fs::remove_all(target,error);return !error;
}
enum class SessionState { Missing,Valid,Corrupt,Unsupported };
bool ParseSession(const Json& json,EditorSessionSnapshot& output)
{
    try
    {
        EditorSessionSnapshot session;
        session.project=json.value("project",std::string{});
        const auto documents=json.value("documents",Json::array()),selection=json.value("selection",Json::array());
        if(!documents.is_array()||!selection.is_array())return false;
        for(const auto& item:documents){if(!item.is_string())return false;session.documents.push_back(item.get<std::string>());}
        session.activeDocument=json.value("active",std::string{});
        for(const auto& item:selection){if(!item.is_number_unsigned())return false;session.selection.push_back(item.get<std::uint64_t>());}
        const auto camera=json.value("camera",Json::array({0.0f,0.0f,0.0f,0.0f,0.0f}));
        if(!camera.is_array()||camera.size()!=5)return false;
        float values[5]{};
        for(std::size_t index=0;index<5;++index){values[index]=camera[index].get<float>();if(!std::isfinite(values[index]))return false;}
        session.cameraX=values[0];session.cameraY=values[1];session.cameraZ=values[2];session.cameraYaw=values[3];session.cameraPitch=values[4];
        session.layoutVersion=json.value("layout_version",1);if(session.layoutVersion<1)return false;
        output=std::move(session);return true;
    }
    catch(const std::exception&){return false;}
}
SessionState ReadSessionFile(const fs::path& path,Json& json,std::string& raw)
{
    std::error_code error;
    if(!fs::exists(path,error))return error?SessionState::Corrupt:SessionState::Missing;
    if(!ReadBytes(path,raw,MaximumManifestBytes))return SessionState::Corrupt;
    try
    {
        json=Json::parse(raw);
        if(!json.is_object()||!json.contains("schema")||!json["schema"].is_number_unsigned())return SessionState::Corrupt;
        if(json["schema"]!=Schema)return SessionState::Unsupported;
        EditorSessionSnapshot ignored;return ParseSession(json,ignored)?SessionState::Valid:SessionState::Corrupt;
    }
    catch(const std::exception&){return SessionState::Corrupt;}
}
bool ReadGroupAt(const fs::path& folder,const fs::path& project,const std::string& id,RecoveryGroup& output,std::string& error)
{
    error.clear();
    if(ReparsePoint(folder)||ReparsePoint(folder/"manifest.json")){error="Editor.RecoveryInvalidPath";return false;}
    std::string text;
    if(!ReadBytes(folder/"manifest.json",text,MaximumManifestBytes)){error="Editor.RecoveryCorrupt";return false;}
    try
    {
        const auto envelope=Json::parse(text);
        if(envelope.at("schema")!=Schema){error="Editor.StorageUnsupportedSchema";return false;}
        const auto& manifest=envelope.at("manifest");
        const auto digest=envelope.at("sha256").get<std::string>();
        if(!Digest(digest)||Sha256(manifest.dump())!=digest){error="Editor.RecoveryCorrupt";return false;}
        if(manifest.at("project").get<std::string>()!=ProjectIdentity(project)){error="Editor.RecoveryProjectMismatch";return false;}
        RecoveryGroup group;group.id=manifest.at("id").get<std::string>();group.createdUtc=manifest.at("created_utc").get<std::string>();
        if(group.id!=id||!SafeToken(group.id)||group.createdUtc.empty())throw std::runtime_error("group metadata");
        const auto& documents=manifest.at("documents");
        if(!documents.is_array()||documents.empty()||documents.size()>1024)throw std::runtime_error("documents");
        std::set<std::string> identifiers;
        for(std::size_t index=0;index<documents.size();++index)
        {
            const auto& item=documents[index];RecoveryDocument document;
            document.documentId=item.at("document_id").get<std::string>();document.kind=item.at("kind").get<std::string>();
            document.relativePath=item.at("relative_path").get<std::string>();document.sourceDigest=item.at("source_digest").get<std::string>();
            if(!item.at("revision").is_number_unsigned()||!item.at("bytes").is_number_unsigned())throw std::runtime_error("revision/size");
            document.revision=item.at("revision").get<std::uint64_t>();
            if(!ValidateDocument(project,document)||!identifiers.insert(document.documentId).second)
            {error="Editor.RecoveryInvalidPath";return false;}
            const auto filename=item.at("file").get<std::string>();
            if(filename!=std::to_string(index)+".snapshot"||ReparsePoint(folder/FromUtf8(filename)))
            {error="Editor.RecoveryInvalidPath";return false;}
            const auto hash=item.at("sha256").get<std::string>();
            if(!Digest(hash)||!ReadBytes(folder/FromUtf8(filename),document.content)||document.content.size()!=item.at("bytes").get<std::uint64_t>()
                ||Sha256(document.content)!=hash){error="Editor.RecoveryCorrupt";return false;}
            group.documents.push_back(std::move(document));
        }
        output=std::move(group);return true;
    }
    catch(const std::exception&){error="Editor.RecoveryCorrupt";return false;}
}
}

bool EditorSessionStore::LoadPreferences(const fs::path& directory,EditorPreferences& output,std::string& error)
{return ProjectWorkspaceSession::LoadPreferences(directory,output,error);}
bool EditorSessionStore::SavePreferences(const fs::path& directory,const EditorPreferences& preferences,std::string& error)
{return ProjectWorkspaceSession::SavePreferences(directory,preferences,error);}
bool EditorSessionStore::LoadSession(const fs::path& directory,EditorSessionSnapshot& output,std::string& error)
{
    error.clear();Json json;std::string raw;
    const auto state=ReadSessionFile(directory/"session.json",json,raw);
    if(state==SessionState::Valid)return ParseSession(json,output);
    if(state==SessionState::Unsupported){error="Editor.StorageUnsupportedSchema";return false;}
    const auto backupState=ReadSessionFile(directory/"session.json.bak",json,raw);
    if(backupState==SessionState::Valid){error="Editor.StorageBackupUsed";return ParseSession(json,output);}
    if(state==SessionState::Missing&&backupState==SessionState::Missing){output={};return true;}
    error=backupState==SessionState::Unsupported?"Editor.StorageUnsupportedSchema":"Editor.StorageCorrupt";return false;
}
bool EditorSessionStore::SaveSession(const fs::path& directory,const EditorSessionSnapshot& session,std::string& error)
{
    error.clear();
    Json json{{"schema",Schema},{"project",session.project},{"documents",session.documents},{"active",session.activeDocument},
        {"selection",session.selection},{"camera",Json::array({session.cameraX,session.cameraY,session.cameraZ,session.cameraYaw,session.cameraPitch})},
        {"layout_version",session.layoutVersion}};
    EditorSessionSnapshot validated;
    if(directory.empty()||!ParseSession(json,validated)){error="Editor.StorageInvalidValue";return false;}
    const auto path=directory/"session.json";Json old;std::string raw;
    const auto state=ReadSessionFile(path,old,raw);
    if(state==SessionState::Unsupported){error="Editor.StorageUnsupportedSchema";return false;}
    if(state!=SessionState::Valid)
    {
        std::string unused;
        if(ReadSessionFile(directory/"session.json.bak",old,unused)==SessionState::Unsupported)
        {error="Editor.StorageUnsupportedSchema";return false;}
    }
    if(state==SessionState::Valid&&!WriteAtomic(directory/"session.json.bak",raw))
    {error="Editor.StorageWriteFailed";return false;}
    if(state==SessionState::Corrupt)
    {
        std::error_code copyError;
        if(!fs::copy_file(path,directory/("session.json.corrupt-"+Token()),fs::copy_options::none,copyError))
        {error="Editor.StorageWriteFailed";return false;}
    }
    if(!WriteAtomic(path,json.dump(2)+"\n")){error="Editor.StorageWriteFailed";return false;}
    return true;
}
bool EditorSessionStore::ClearSession(const fs::path& directory,std::string& error)
{
    error.clear();std::error_code fileError;
    for(const auto* file:{"session.json","session.json.bak"})
    {fs::remove(directory/file,fileError);if(fileError){error="Editor.StorageWriteFailed";return false;}}
    return true;
}
fs::path EditorSessionStore::ProjectDirectory(const fs::path& projectRoot)
{
    const auto root=Canonical(projectRoot);
    return root.empty()?fs::path{}:root/"out"/"Editor"/UserDataDirectory().filename();
}
bool EditorSessionStore::WriteRecoveryGroup(const fs::path& directory,const fs::path& projectRoot,
    const std::vector<RecoveryDocument>& documents,std::string& error,std::uint64_t maximumTotalBytes,std::size_t maximumRevisions,
    const std::vector<std::string>& protectedGroups)
{
    error.clear();const auto project=Canonical(projectRoot);fs::path recovery;
    if(project.empty()||!RecoveryRoot(directory,recovery)){error="Editor.RecoveryInvalidPath";return false;}
    if(documents.empty()||documents.size()>1024||maximumRevisions==0){error="Editor.RecoveryInvalidDocument";return false;}
    const auto id=Token(),created=UtcNow();Json metadata=Json::array();std::set<std::string> identifiers;
    std::uint64_t groupBytes=0;
    for(std::size_t index=0;index<documents.size();++index)
    {
        const auto& document=documents[index];
        if(!ValidateDocument(project,document)||!identifiers.insert(document.documentId).second)
        {error="Editor.RecoveryInvalidDocument";return false;}
        if(document.content.size()>maximumTotalBytes-groupBytes){error="Editor.RecoveryTooLarge";return false;}
        groupBytes+=document.content.size();const auto digest=Sha256(document.content);
        if(!Digest(digest)){error="Editor.RecoveryWriteFailed";return false;}
        metadata.push_back({{"document_id",document.documentId},{"kind",document.kind},{"relative_path",document.relativePath},
            {"source_digest",document.sourceDigest},{"revision",document.revision},{"file",std::to_string(index)+".snapshot"},
            {"bytes",document.content.size()},{"sha256",digest}});
    }
    const Json manifest{{"id",id},{"created_utc",created},{"project",ProjectIdentity(project)},{"documents",metadata}};
    const auto manifestHash=Sha256(manifest.dump());
    if(!Digest(manifestHash)){error="Editor.RecoveryWriteFailed";return false;}
    const auto text=Json{{"schema",Schema},{"manifest",manifest},{"sha256",manifestHash}}.dump(2)+"\n";
    if(text.size()>maximumTotalBytes-groupBytes||text.size()>MaximumManifestBytes){error="Editor.RecoveryTooLarge";return false;}
    groupBytes+=text.size();
    const auto groups=recovery/"groups";
    if(ReparsePoint(groups)||!Inside(recovery,Canonical(groups))){error="Editor.RecoveryInvalidPath";return false;}
    std::uint64_t existingBytes=0;
    if(!DirectoryBytes(recovery,existingBytes)){error="Editor.RecoveryInvalidPath";return false;}
    std::string listError;auto existing=ListRecoveryGroups(directory,project,listError);
    std::vector<std::string> evict;
    auto retained=existing.size()+1;
    // 候选发布前只计划淘汰；失败或过大不会先删掉最后一个有效组。
    for(auto it=existing.rbegin();it!=existing.rend()&&(existingBytes>maximumTotalBytes-groupBytes||retained>maximumRevisions);++it)
    {
        if(std::find(protectedGroups.begin(),protectedGroups.end(),it->id)!=protectedGroups.end())continue;
        std::uint64_t size=0;
        if(!DirectoryBytes(groups/FromUtf8(it->id),size)){error="Editor.RecoveryInvalidPath";return false;}
        existingBytes=size>existingBytes?0:existingBytes-size;--retained;evict.push_back(it->id);
    }
    if(existingBytes>maximumTotalBytes-groupBytes||retained>maximumRevisions){error="Editor.RecoveryBudgetBlocked";return false;}
    std::error_code fileError;fs::create_directories(groups,fileError);
    if(fileError){error="Editor.RecoveryWriteFailed";return false;}
    const auto candidate=groups/(".candidate-"+id),target=groups/id;
    if(!fs::create_directory(candidate,fileError)||fileError){error="Editor.RecoveryWriteFailed";return false;}
    struct CandidateGuard
    {
        fs::path root,path;bool published=false;
        ~CandidateGuard(){if(!published)RemoveGroupDirectory(root,path);}
    } guard{recovery,candidate};
    for(std::size_t index=0;index<documents.size();++index)
        if(!WriteFileBytes(candidate/(std::to_string(index)+".snapshot"),documents[index].content))
        {error="Editor.RecoveryWriteFailed";return false;}
    // manifest 最后落盘；整个目录同卷重命名后才进入可列举集合。
    if(!WriteFileBytes(candidate/"manifest.json",text)){error="Editor.RecoveryWriteFailed";return false;}
    RecoveryGroup verified;
    if(!ReadGroupAt(candidate,project,id,verified,error))return false;
    if(!MoveFileExW(candidate.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)){error="Editor.RecoveryWriteFailed";return false;}
    guard.published=true;
    for(const auto& obsolete:evict)
        if(!RemoveGroupDirectory(recovery,groups/FromUtf8(obsolete))){error="Editor.RecoveryDiscardFailed";return false;}
    return true;
}
std::vector<RecoveryGroup> EditorSessionStore::ListRecoveryGroups(const fs::path& directory,const fs::path& projectRoot,std::string& error)
{
    error.clear();std::vector<RecoveryGroup> result;fs::path recovery;const auto project=Canonical(projectRoot);
    if(project.empty()||!RecoveryRoot(directory,recovery)){error="Editor.RecoveryInvalidPath";return result;}
    const auto groups=recovery/"groups";std::error_code fileError;
    if(!fs::exists(groups,fileError)){if(fileError)error="Editor.RecoveryCorrupt";return result;}
    if(ReparsePoint(groups)){error="Editor.RecoveryInvalidPath";return result;}
    for(fs::directory_iterator it(groups,fileError),end;it!=end&&!fileError;it.increment(fileError))
    {
        const auto id=Utf8(it->path().filename());if(!SafeToken(id))continue;
        if(!it->is_directory(fileError))continue;
        RecoveryGroup group;std::string groupError;
        if(ReadGroupAt(it->path(),project,id,group,groupError))result.push_back(std::move(group));
        else if(error.empty())error=std::move(groupError);
    }
    if(fileError)error="Editor.RecoveryCorrupt";
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id>b.id;});return result;
}
bool EditorSessionStore::ReadRecoveryGroup(const fs::path& directory,const fs::path& projectRoot,const std::string& id,
    RecoveryGroup& output,std::string& error)
{
    error.clear();fs::path recovery;const auto project=Canonical(projectRoot);
    if(!SafeToken(id)||project.empty()||!RecoveryRoot(directory,recovery)||ReparsePoint(recovery/"groups"))
    {error="Editor.RecoveryInvalidPath";return false;}
    const auto folder=recovery/"groups"/FromUtf8(id);
    if(!Inside(recovery,Canonical(folder))){error="Editor.RecoveryInvalidPath";return false;}
    std::error_code fileError;if(!fs::exists(folder,fileError)){error="Editor.RecoveryNotFound";return false;}
    return ReadGroupAt(folder,project,id,output,error);
}
bool EditorSessionStore::DiscardRecoveryGroup(const fs::path& directory,const std::string& id,std::string& error)
{
    error.clear();fs::path recovery;
    if(!SafeToken(id)||!RecoveryRoot(directory,recovery)||ReparsePoint(recovery/"groups"))
    {error="Editor.RecoveryInvalidPath";return false;}
    if(!RemoveGroupDirectory(recovery,recovery/"groups"/FromUtf8(id))){error="Editor.RecoveryDiscardFailed";return false;}
    return true;
}
bool EditorSessionStore::CleanupAbandonedCandidates(const fs::path& directory,std::string& error)
{
    error.clear();
    const auto noActiveMarker=[&](){
        const auto path=directory/"process.json";std::error_code fileError;
        if(ReparsePoint(path)){error="Editor.RecoveryInvalidPath";return false;}
        const bool exists=fs::exists(path,fileError);
        if(fileError){error="Editor.StorageCorrupt";return false;}
        if(!exists)return true;
        CrashMarker marker;
        if(!ReadCrashMarker(directory,marker,error)){if(error.empty())error="Editor.StorageCorrupt";return false;}
        if(IsProcessActive(marker)){error="Editor.RecoveryCleanupActive";return false;}
        return true;
    };
    if(!noActiveMarker())return false;
    fs::path recovery;
    if(!RecoveryRoot(directory,recovery)){error="Editor.RecoveryInvalidPath";return false;}
    const auto groups=recovery/"groups";std::error_code fileError;
    if(ReparsePoint(groups)||!Inside(recovery,Canonical(groups))){error="Editor.RecoveryInvalidPath";return false;}
    if(!fs::exists(groups,fileError))
    {if(fileError){error="Editor.RecoveryInvalidPath";return false;}return true;}
    std::vector<fs::path> abandoned;
    for(fs::directory_iterator it(groups,fileError),end;it!=end&&!fileError;it.increment(fileError))
    {
        if(!GeneratedCandidateName(Utf8(it->path().filename())))continue;
        // 先校验所有候选，遇到链接时整次拒绝，不进行部分递归清理。
        if(ReparsePoint(it->path())){error="Editor.RecoveryInvalidPath";return false;}
        const bool isDirectory=it->is_directory(fileError);
        if(fileError){error="Editor.RecoveryInvalidPath";return false;}
        if(!isDirectory)continue;
        const auto target=Canonical(it->path());std::uint64_t unused=0;
        if(target==groups||!Inside(groups,target)||!DirectoryBytes(it->path(),unused))
        {error="Editor.RecoveryInvalidPath";return false;}
        abandoned.push_back(it->path());
    }
    if(fileError){error="Editor.RecoveryInvalidPath";return false;}
    for(const auto& candidate:abandoned)
    {
        if(!noActiveMarker())return false;
        if(!Inside(groups,Canonical(candidate))){error="Editor.RecoveryInvalidPath";return false;}
        if(!RemoveGroupDirectory(recovery,candidate)){error="Editor.RecoveryDiscardFailed";return false;}
    }
    return true;
}
bool EditorSessionStore::WriteRecovery(const fs::path& directory,const std::string& documentId,const std::string& content,
    std::uint32_t revision,std::uint64_t maximumTotalBytes,std::string& error)
{
    error.clear();fs::path recovery;
    if(!SafeToken(documentId)||!RecoveryRoot(directory,recovery)){error="Editor.RecoveryInvalidPath";return false;}
    if(content.size()>maximumTotalBytes){error="Editor.RecoveryTooLarge";return false;}
    auto entries=ListRecovery(directory);std::uint64_t bytes=0;
    if(!DirectoryBytes(recovery,bytes)){error="Editor.RecoveryInvalidPath";return false;}
    std::vector<RecoveryEntry> matching;
    for(const auto& entry:entries)
        if(entry.documentId==documentId){if(entry.revision==revision)bytes-=entry.bytes;else matching.push_back(entry);}
    std::sort(matching.begin(),matching.end(),[](const auto& a,const auto& b){return a.revision<b.revision;});
    std::vector<RecoveryEntry> evict;
    for(const auto& entry:matching)
    {
        if(bytes<=maximumTotalBytes-content.size()&&matching.size()+1-evict.size()<=MaximumRecoveryRevisions)break;
        bytes-=entry.bytes;evict.push_back(entry);
    }
    if(bytes>maximumTotalBytes-content.size()){error="Editor.RecoveryBudgetBlocked";return false;}
    if(!WriteAtomic(recovery/(documentId+"."+std::to_string(revision)+".json"),content)){error="Editor.RecoveryWriteFailed";return false;}
    std::error_code fileError;
    for(const auto& entry:evict)
    {fs::remove(recovery/(entry.documentId+"."+std::to_string(entry.revision)+".json"),fileError);if(fileError){error="Editor.RecoveryDiscardFailed";return false;}}
    return true;
}
std::vector<EditorSessionStore::RecoveryEntry> EditorSessionStore::ListRecovery(const fs::path& directory)
{
    std::vector<RecoveryEntry> entries;std::error_code fileError;fs::path recovery;
    if(!RecoveryRoot(directory,recovery))return entries;
    for(fs::directory_iterator it(recovery,fileError),end;it!=end&&!fileError;it.increment(fileError))
    {
        if(!it->is_regular_file(fileError)||ReparsePoint(it->path()))continue;
        const auto name=Utf8(it->path().filename());
        if(name.size()<6||name.compare(name.size()-5,5,".json")!=0)continue;
        const auto stem=name.substr(0,name.size()-5);const auto dot=stem.find_last_of('.');
        if(dot==std::string::npos)continue;
        RecoveryEntry value;value.documentId=stem.substr(0,dot);if(!SafeToken(value.documentId))continue;
        const auto revision=stem.substr(dot+1);const auto parsed=std::from_chars(revision.data(),revision.data()+revision.size(),value.revision);
        if(parsed.ec!=std::errc{}||parsed.ptr!=revision.data()+revision.size())continue;
        value.bytes=it->file_size(fileError);if(!fileError)entries.push_back(std::move(value));
    }
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.documentId==b.documentId?a.revision<b.revision:a.documentId<b.documentId;});
    return entries;
}
bool EditorSessionStore::RecoveryDebounce::Due(std::uint64_t nowSeconds,std::uint64_t revision) const
{return !armed_||(revision!=lastRevision_&&nowSeconds>=lastSeconds_&&nowSeconds-lastSeconds_>=interval_);}
void EditorSessionStore::RecoveryDebounce::Mark(std::uint64_t nowSeconds,std::uint64_t revision)
{armed_=true;lastSeconds_=nowSeconds;lastRevision_=revision;}
std::uint64_t EditorSessionStore::RecoveryBytes(const fs::path& directory,const std::string& documentId)
{std::uint64_t bytes=0;for(const auto& entry:ListRecovery(directory))if(entry.documentId==documentId)bytes+=entry.bytes;return bytes;}
std::size_t EditorSessionStore::RecoveryCount(const fs::path& directory,const std::string& documentId)
{std::size_t count=0;for(const auto& entry:ListRecovery(directory))if(entry.documentId==documentId)++count;return count;}
bool EditorSessionStore::WriteCrashMarker(const fs::path& directory,const CrashMarker& marker,std::string& error)
{
    error.clear();const Json json{{"schema",Schema},{"pid",marker.processId},{"creation",marker.processCreation},{"started",marker.startedUtc}};
    if(!WriteAtomic(directory/"process.json",json.dump(2)+"\n")){error="Editor.StorageWriteFailed";return false;}return true;
}
bool EditorSessionStore::ReadCrashMarker(const fs::path& directory,CrashMarker& marker,std::string& error)
{
    error.clear();std::string text;std::error_code fileError;
    if(!fs::exists(directory/"process.json",fileError))return false;
    if(!ReadBytes(directory/"process.json",text,MaximumManifestBytes)){error="Editor.StorageCorrupt";return false;}
    try
    {
        const auto json=Json::parse(text);
        if(json.at("schema")!=Schema){error="Editor.StorageUnsupportedSchema";return false;}
        marker.processId=json.at("pid").get<std::uint32_t>();marker.processCreation=json.at("creation").get<std::uint64_t>();
        marker.startedUtc=json.at("started").get<std::string>();return true;
    }
    catch(const std::exception&){error="Editor.StorageCorrupt";return false;}
}
bool EditorSessionStore::ClearCrashMarker(const fs::path& directory,std::string& error)
{
    error.clear();std::error_code fileError;fs::remove(directory/"process.json",fileError);
    if(fileError){error="Editor.StorageWriteFailed";return false;}return true;
}
fs::path EditorSessionStore::EngineRootFromExecutable()
{return ProjectWorkspaceSession::EngineRootFromExecutable();}
fs::path EditorSessionStore::UserDataDirectory()
{return ProjectWorkspaceSession::UserDataDirectory(EngineRootFromExecutable());}
std::uint64_t EditorSessionStore::CurrentProcessCreation()
{
    FILETIME creation{},exit{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))return 0;
    return (static_cast<std::uint64_t>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
}
EditorSessionStore::StartupState EditorSessionStore::InspectStartup(const fs::path& directory,CrashMarker& marker,std::string& error)
{
    marker={};error.clear();
    if(!ReadCrashMarker(directory,marker,error))return error.empty()?StartupState::Clean:StartupState::UncleanExit;
    return IsProcessActive(marker)?StartupState::ActiveInstance:StartupState::UncleanExit;
}
bool EditorSessionStore::IsProcessActive(const CrashMarker& marker)
{
    if(!marker.processId)return false;
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,marker.processId);
    if(!process)return false;
    struct Guard{HANDLE value;~Guard(){CloseHandle(value);}} guard{process};
    if(WaitForSingleObject(process,0)==WAIT_OBJECT_0)return false;
    FILETIME creation{},exit{},kernel{},user{};
    if(!GetProcessTimes(process,&creation,&exit,&kernel,&user))return true;
    const auto value=(static_cast<std::uint64_t>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
    return marker.processCreation==0||value==marker.processCreation;
}
}
