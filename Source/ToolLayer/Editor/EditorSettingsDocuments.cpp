#include "EditorSettingsDocuments.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <Windows.h>

namespace se
{
namespace
{
namespace fs=std::filesystem;
const char* Id(EditorSettingsKind kind){return kind==EditorSettingsKind::Project?"project-settings":"build-settings";}
const wchar_t* Filename(EditorSettingsKind kind){return kind==EditorSettingsKind::Project?L"project.json":L"build.json";}
bool Fail(std::string& error,const char* key){error=key;return false;}
bool SafePath(const fs::path& path,EditorSettingsKind kind)
{
    if(!path.is_absolute()||CompareStringOrdinal(path.filename().c_str(),-1,Filename(kind),-1,TRUE)!=CSTR_EQUAL)return false;
    std::error_code error;if(!fs::is_directory(path.parent_path(),error)||error)return false;
    fs::path part;
    for(const auto& component:path)
    {
        part/=component;const auto attributes=GetFileAttributesW(part.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES){if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)return false;}
        else{const auto failure=GetLastError();if(failure!=ERROR_FILE_NOT_FOUND&&failure!=ERROR_PATH_NOT_FOUND)return false;}
    }
    return true;
}
bool ReadSource(const fs::path& path,bool& exists,std::string& text,std::string& error)
{
    exists=false;text.clear();std::error_code failure;
    exists=fs::exists(path,failure);if(failure)return Fail(error,"Project.ReadFailed");
    if(!exists)return true;
    if(!fs::is_regular_file(path,failure)||failure||fs::file_size(path,failure)>4*1024*1024||failure)return Fail(error,"Project.ReadFailed");
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes))return Fail(error,"Project.ReadFailed");
    text.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());return true;
}
std::string Digest(std::string_view text)
{return asset_io::Hash({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
bool Decode(EditorSettingsKind kind,std::string_view text,ProjectSettings& project,BuildSettings& build,std::string& content,std::string& error)
{
    return kind==EditorSettingsKind::Project
        ?ProjectSettingsService::DecodeProject(text,project,error)&&ProjectSettingsService::MergeProject(text,project,content,error)
        :ProjectSettingsService::DecodeBuild(text,build,error)&&ProjectSettingsService::MergeBuild(text,build,content,error);
}
}

bool EditorSettingsDocument::Load(const fs::path& requested,std::string& error,bool discardUnsaved)
{
    error.clear();if(Dirty()&&!discardUnsaved)return Fail(error,"Editor.UnsavedChanges");
    try
    {
        const auto path=fs::absolute(requested).lexically_normal();
        if(!SafePath(path,kind_))return Fail(error,"Project.PathOutsideRoot");
        bool exists=false;std::string source;
        if(!ReadSource(path,exists,source,error))return false;
        if(!exists&&kind_==EditorSettingsKind::Project)return Fail(error,"Project.ReadFailed");
        const auto sourceDigest=exists?Digest(source):std::string{};
        if(!exists&&!ProjectSettingsService::MergeBuild({},BuildSettings{},source,error))return false;
        ProjectSettings project;BuildSettings build;std::string content;
        if(!Decode(kind_,source,project,build,content,error))return false;
        if(content!=content_&&revision_==UINT64_MAX)return Fail(error,"Editor.RecoveryInvalidDocument");
        if(content!=content_)++revision_;
        path_=path;project_=std::move(project);build_=std::move(build);content_=std::move(content);savedContent_=content_;
        sourceExists_=exists;sourceDigest_=sourceDigest;loaded_=true;return true;
    }
    catch(...){return Fail(error,"Project.ReadFailed");}
}
bool EditorSettingsDocument::Set(const ProjectSettings& settings,std::string& error)
{
    error.clear();if(!loaded_||kind_!=EditorSettingsKind::Project)return Fail(error,"Project.InvalidManifest");
    std::string content;ProjectSettings project;
    if(!ProjectSettingsService::MergeProject(content_,settings,content,error)||!ProjectSettingsService::DecodeProject(content,project,error))return false;
    if(content!=content_&&revision_==UINT64_MAX)return Fail(error,"Editor.RecoveryInvalidDocument");
    if(content!=content_)++revision_;content_=std::move(content);project_=std::move(project);return true;
}
bool EditorSettingsDocument::Set(const BuildSettings& settings,std::string& error)
{
    error.clear();if(!loaded_||kind_!=EditorSettingsKind::Build)return Fail(error,"Project.InvalidBuildSettings");
    std::string content;BuildSettings build;
    if(!ProjectSettingsService::MergeBuild(content_,settings,content,error)||!ProjectSettingsService::DecodeBuild(content,build,error))return false;
    if(content!=content_&&revision_==UINT64_MAX)return Fail(error,"Editor.RecoveryInvalidDocument");
    if(content!=content_)++revision_;content_=std::move(content);build_=std::move(build);return true;
}
bool EditorSettingsDocument::CheckSource(std::string& error)const
{
    if(!loaded_)return Fail(error,"Editor.RecoveryInvalidDocument");
    if(!SafePath(path_,kind_))return Fail(error,"Project.PathOutsideRoot");
    bool exists=false;std::string source;
    if(!ReadSource(path_,exists,source,error))return false;
    if(exists!=sourceExists_||(exists&&Digest(source)!=sourceDigest_))return Fail(error,"Editor.ExternalChange");
    return true;
}
bool EditorSettingsDocument::Save(std::string& error,std::string& detail)
{
    error.clear();detail.clear();
    try
    {
        if(!loaded_)return Fail(error,"Editor.RecoveryInvalidDocument");
        const bool valid=kind_==EditorSettingsKind::Project
            ?ProjectSettingsService::ValidateProject(path_.parent_path(),project_,error,detail)
            :ProjectSettingsService::ValidateBuild(path_.parent_path(),build_,error,detail);
        if(!valid)return false;
        if(!CheckSource(error)){detail=asset_io::Utf8(path_);return false;}
        if(!Dirty()&&sourceExists_)return true;
        std::string publication=content_;
        if(kind_==EditorSettingsKind::Project&&!ProjectSettingsService::PrepareProjectSave(path_.parent_path(),content_,project_,publication,error))return false;
        if(publication!=content_&&revision_==UINT64_MAX)return Fail(error,"Editor.RecoveryInvalidDocument");
        if(!asset_io::WriteAtomic(path_,{reinterpret_cast<const std::uint8_t*>(publication.data()),publication.size()},sourceExists_))
        {detail=asset_io::Utf8(path_);return Fail(error,"Project.WriteFailed");}
        if(publication!=content_)++revision_;content_=std::move(publication);
        sourceExists_=true;sourceDigest_=Digest(content_);savedContent_=content_;return true;
    }
    catch(...){return Fail(error,"Project.WriteFailed");}
}
bool EditorSettingsDocument::Snapshot(RecoveryDocument& document,std::string& error)const
{
    error.clear();if(!loaded_)return Fail(error,"Editor.RecoveryInvalidDocument");
    document={Id(kind_),Id(kind_),kind_==EditorSettingsKind::Project?"project.json":"build.json",sourceDigest_,content_,revision_};return true;
}
bool EditorSettingsDocument::Restore(const RecoveryDocument& document,bool apply,std::string& error)
{
    error.clear();
    try
    {
        if(!loaded_||document.documentId!=Id(kind_)||document.kind!=Id(kind_)
            ||document.relativePath!=(kind_==EditorSettingsKind::Project?"project.json":"build.json"))
            return Fail(error,"Editor.RecoveryInvalidDocument");
        if(document.sourceDigest!=sourceDigest_)return Fail(error,"Editor.ExternalChange");
        if(!CheckSource(error))return false;
        ProjectSettings project;BuildSettings build;std::string content;
        if(!Decode(kind_,document.content,project,build,content,error))return false;
        if(content!=content_&&revision_==UINT64_MAX)return Fail(error,"Editor.RecoveryInvalidDocument");
        if(!apply)return true;
        if(content!=content_)++revision_;content_=std::move(content);project_=std::move(project);build_=std::move(build);return true;
    }
    catch(...){return Fail(error,"Editor.RecoveryInvalidDocument");}
}
}
