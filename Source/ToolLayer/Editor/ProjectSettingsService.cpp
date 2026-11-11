#include "ProjectSettingsService.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

namespace se
{
namespace
{
using Json=nlohmann::json;
bool WriteAtomic(const std::filesystem::path& path,const std::string& text)
{
    return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
}
bool ReadText(const std::filesystem::path& path,std::string& output,std::string& errorKey)
{
    std::error_code error;
    if(std::filesystem::file_size(path,error)>4*1024*1024||error){errorKey="Project.ReadFailed";return false;}
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)){errorKey="Project.ReadFailed";return false;}
    output.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());return true;
}
bool Parse(std::string_view text,bool build,Json& output,std::string& errorKey)
{
    errorKey.clear();const auto invalid=build?"Project.InvalidBuildSettings":"Project.InvalidManifest";
    try
    {
        if(text.empty()||text.size()>4*1024*1024){errorKey=invalid;return false;}
        bool duplicate=false;std::vector<std::set<std::string>> keys;
        auto parsed=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t event,Json& value){
            if(depth>32)throw std::runtime_error("Project.InvalidManifest");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            else if(event==Json::parse_event_t::key&&!keys.empty()&&!keys.back().insert(value.get<std::string>()).second)duplicate=true;
            else if(event==Json::parse_event_t::object_end)keys.pop_back();return true;
        });
        if(duplicate||!parsed.is_object()||parsed.value("format",std::string{})!=(build?"SelfEngine.Build":"SelfEngine.Project"))
        {errorKey=invalid;return false;}
        if(!parsed.contains("version")||!parsed.at("version").is_number_integer()||parsed.at("version")!=1)
        {errorKey="Project.UnsupportedVersion";return false;}
        output=std::move(parsed);return true;
    }
    catch(...){errorKey=invalid;return false;}
}
bool InvalidName(const std::string& name)
{
    return name.empty()||name.size()>128||name.find_first_of("<>:\"/\\|?*")!=std::string::npos;
}
std::filesystem::path FromUtf8(const std::string& text)
{
    if(text.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(size<=0)return {};
    std::wstring wide(static_cast<std::size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),wide.data(),size);
    return std::filesystem::path(wide);
}
bool RelativeInside(const std::filesystem::path& root,const std::string& value,std::filesystem::path& output)
{
    if(value.empty())return false;
    const std::filesystem::path requested=FromUtf8(value);
    if(requested.is_absolute()||requested.root_name()!=std::filesystem::path{})return false;
    std::error_code error;
    const auto combined=std::filesystem::weakly_canonical(root/requested,error);
    if(error||combined.empty())return false;
    const auto canonicalRoot=std::filesystem::weakly_canonical(root,error);
    if(error||canonicalRoot.empty())return false;
    auto rootText=canonicalRoot.native();auto pathText=combined.native();
    std::transform(rootText.begin(),rootText.end(),rootText.begin(),[](wchar_t ch){return static_cast<wchar_t>(towlower(ch));});
    std::transform(pathText.begin(),pathText.end(),pathText.begin(),[](wchar_t ch){return static_cast<wchar_t>(towlower(ch));});
    if(pathText.size()<=rootText.size()||pathText.compare(0,rootText.size(),rootText)!=0)return false;
    if(pathText[rootText.size()]!=L'\\'&&pathText[rootText.size()]!=L'/')return false;
    output=combined;
    return true;
}
}
bool ProjectSettingsService::DecodeProject(std::string_view text,ProjectSettings& output,std::string& errorKey)
{
    errorKey.clear();Json json;
    if(!Parse(text,false,json,errorKey))return false;
    try
    {
        ProjectSettings settings;
        settings.name=json.at("name").get<std::string>();
        settings.contentRoot=json.at("contentRoot").get<std::string>();
        settings.entryScene=json.at("entryScene").get<std::string>();
        if(json.contains("assets")&&json.at("assets").is_array())
        {
            for(const auto& item:json.at("assets"))
            {
                if(!item.is_object())continue;
                const auto id=item.value("id",std::string{});
                const auto type=item.value("type",std::string{});
                if(!id.empty())settings.assets.push_back(type.empty()?id:id+" ("+type+")");
            }
        }
        output=std::move(settings);return true;
    }
    catch(...){errorKey="Project.InvalidManifest";return false;}
}
bool ProjectSettingsService::ReadProject(const std::filesystem::path& manifest,ProjectSettings& output,std::string& errorKey)
{
    errorKey.clear();std::string text;ProjectSettings settings;
    if(!ReadText(manifest,text,errorKey)||!DecodeProject(text,settings,errorKey))return false;
    if(settings.name.empty()||settings.contentRoot.empty()||settings.entryScene.empty()){errorKey="Project.InvalidManifest";return false;}
    output=std::move(settings);return true;
}
bool ProjectSettingsService::MergeProject(std::string_view original,const ProjectSettings& settings,std::string& output,std::string& errorKey)
{
    Json json;if(!Parse(original,false,json,errorKey))return false;
    try
    {
        json["name"]=settings.name;json["contentRoot"]=settings.contentRoot;json["entryScene"]=settings.entryScene;
        output=json.dump(2)+"\n";return true;
    }
    catch(...){errorKey="Project.InvalidManifest";return false;}
}
bool ProjectSettingsService::PrepareProjectSave(const std::filesystem::path& projectRoot,std::string_view original,
    const ProjectSettings& settings,std::string& output,std::string& errorKey)
{
    std::string merged;if(!MergeProject(original,settings,merged,errorKey))return false;
    std::filesystem::path entry;
    std::error_code error;
    const auto absoluteRoot=std::filesystem::absolute(projectRoot.empty()?std::filesystem::path("."):projectRoot,error);
    if(error||!RelativeInside(absoluteRoot,settings.entryScene,entry)||!std::filesystem::is_regular_file(entry,error)||error)
    {errorKey="Project.InvalidEntryScene";return false;}
    Json json;if(!Parse(merged,false,json,errorKey))return false;
    try
    {
        if(!json.contains("scenes"))json["scenes"]=Json::array();
        auto& scenes=json["scenes"];
        if(!scenes.is_array()){errorKey="Project.InvalidManifest";return false;}
        bool registered=false;
        for(const auto& value:scenes)
        {
            std::filesystem::path scene;
            if(!value.is_string()||!RelativeInside(absoluteRoot,value.get<std::string>(),scene))
            {errorKey="Project.InvalidManifest";return false;}
            if(CompareStringOrdinal(scene.c_str(),-1,entry.c_str(),-1,TRUE)==CSTR_EQUAL)registered=true;
        }
        if(!registered)
        {
            if(scenes.size()>=10000){errorKey="Project.InvalidManifest";return false;}
            const auto root=std::filesystem::weakly_canonical(absoluteRoot,error);
            if(error){errorKey="Project.InvalidEntryScene";return false;}
            const auto relative=entry.lexically_relative(root);
            if(relative.empty()||relative.is_absolute()||*relative.begin()=="..")
            {errorKey="Project.InvalidEntryScene";return false;}
            scenes.push_back(asset_io::Utf8(relative));
        }
        output=json.dump(2)+"\n";return true;
    }
    catch(...){errorKey="Project.InvalidManifest";return false;}
}
bool ProjectSettingsService::WriteProject(const std::filesystem::path& manifest,const ProjectSettings& settings,std::string& errorKey)
{
    errorKey.clear();
    if(InvalidName(settings.name)){errorKey="Project.InvalidName";return false;}
    std::string original,text;
    if(!ReadText(manifest,original,errorKey)||!PrepareProjectSave(manifest.parent_path(),original,settings,text,errorKey))return false;
    if(!WriteAtomic(manifest,text)){errorKey="Project.WriteFailed";return false;}
    return true;
}
bool ProjectSettingsService::ReadBuild(const std::filesystem::path& buildJson,BuildSettings& output,std::string& errorKey)
{
    errorKey.clear();
    std::error_code fileError;
    if(!std::filesystem::exists(buildJson,fileError)){errorKey=fileError?"Project.ReadFailed":"Project.BuildSettingsMissing";return false;}
    std::string text;if(!ReadText(buildJson,text,errorKey))return false;
    return DecodeBuild(text,output,errorKey);
}
bool ProjectSettingsService::DecodeBuild(std::string_view text,BuildSettings& output,std::string& errorKey)
{
    errorKey.clear();
    Json json;
    if(!Parse(text,true,json,errorKey))return false;
    try
    {
        BuildSettings settings;
        settings.scriptsProject=json.value("scriptsProject",std::string{});
        settings.scriptType=json.value("scriptType",std::string{});
        settings.bindings=json.value("bindings",std::string{});
        settings.entryUi=json.value("entryUi",std::string{});
        output=std::move(settings);return true;
    }
    catch(...){errorKey="Project.InvalidBuildSettings";return false;}
}
bool ProjectSettingsService::MergeBuild(std::string_view original,const BuildSettings& settings,std::string& output,std::string& errorKey)
{
    errorKey.clear();Json json={{"format","SelfEngine.Build"},{"version",1}};
    if(!original.empty()&&!Parse(original,true,json,errorKey))return false;
    try
    {
        json["scriptsProject"]=settings.scriptsProject;json["scriptType"]=settings.scriptType;
        json["bindings"]=settings.bindings;json["entryUi"]=settings.entryUi;
        output=json.dump(2)+"\n";return true;
    }
    catch(...){errorKey="Project.InvalidBuildSettings";return false;}
}
bool ProjectSettingsService::WriteBuild(const std::filesystem::path& buildJson,const BuildSettings& settings,std::string& errorKey)
{
    errorKey.clear();
    std::error_code error;const bool exists=std::filesystem::exists(buildJson,error);
    if(error){errorKey="Project.ReadFailed";return false;}
    std::string original,text;
    if(exists&&(!ReadText(buildJson,original,errorKey)||original.empty()))
    {if(errorKey.empty())errorKey="Project.InvalidBuildSettings";return false;}
    if(!MergeBuild(original,settings,text,errorKey))return false;
    if(!WriteAtomic(buildJson,text)){errorKey="Project.WriteFailed";return false;}
    return true;
}
std::vector<std::string> ProjectSettingsService::MissingBuildFields(const BuildSettings& settings)
{
    std::vector<std::string> missing;
    if(settings.scriptsProject.empty())missing.push_back("scriptsProject");
    if(settings.scriptType.empty())missing.push_back("scriptType");
    return missing;
}
bool ProjectSettingsService::ValidateProject(const std::filesystem::path& projectRoot,const ProjectSettings& settings,
    std::string& errorKey,std::string& detail)
{
    errorKey.clear();detail.clear();
    if(InvalidName(settings.name)){errorKey="Project.InvalidName";detail=settings.name;return false;}
    std::filesystem::path contentRoot;
    if(!RelativeInside(projectRoot,settings.contentRoot,contentRoot)){errorKey="Project.InvalidContentRoot";detail=settings.contentRoot;return false;}
    std::error_code error;
    if(!std::filesystem::is_directory(contentRoot,error)){errorKey="Project.InvalidContentRoot";detail=settings.contentRoot;return false;}
    std::filesystem::path scene;
    if(!RelativeInside(projectRoot,settings.entryScene,scene)){errorKey="Project.InvalidEntryScene";detail=settings.entryScene;return false;}
    if(!std::filesystem::is_regular_file(scene,error)){errorKey="Project.InvalidEntryScene";detail=settings.entryScene;return false;}
    return true;
}
bool ProjectSettingsService::ValidateBuild(const std::filesystem::path& projectRoot,const BuildSettings& settings,
    std::string& errorKey,std::string& detail)
{
    errorKey.clear();detail.clear();
    std::error_code error;
    if(!settings.scriptsProject.empty())
    {
        std::filesystem::path scripts;
        if(!RelativeInside(projectRoot,settings.scriptsProject,scripts)
            ||scripts.extension()!=".csproj"||!std::filesystem::is_regular_file(scripts,error))
        {errorKey="Project.InvalidScriptsProject";detail=settings.scriptsProject;return false;}
        if(settings.scriptType.empty()){errorKey="Project.InvalidBuildSettings";detail="scriptType";return false;}
    }
    if(!settings.bindings.empty())
    {
        std::filesystem::path bindings;
        if(!RelativeInside(projectRoot,settings.bindings,bindings)||!std::filesystem::is_regular_file(bindings,error))
        {errorKey="Project.InvalidBindings";detail=settings.bindings;return false;}
    }
    if(!settings.entryUi.empty())
    {
        std::filesystem::path ui;
        if(!RelativeInside(projectRoot,settings.entryUi,ui)||!std::filesystem::is_regular_file(ui,error))
        {errorKey="Project.InvalidEntryUi";detail=settings.entryUi;return false;}
    }
    return true;
}
}
