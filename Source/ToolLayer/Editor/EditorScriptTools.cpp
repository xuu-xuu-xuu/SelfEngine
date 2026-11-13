#include "EditorScriptTools.h"
#include "EditorProcessRunner.h"
#include "CoreLayer/Utf.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ToolLayer/Tools/Project.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <set>

namespace se
{
namespace
{
namespace fs=std::filesystem;
using Json=nlohmann::json;
bool Fail(std::string& key,std::string& detail,const char* value,const fs::path& path={})
{key=value;detail=asset_io::Utf8(path);return false;}
bool Safe(const fs::path& root,const fs::path& path)
{
    if(!path.is_absolute()||!asset_io::Within(root,path))return false;
    fs::path part;
    for(const auto& component:path)
    {
        part/=component;const DWORD flags=GetFileAttributesW(part.c_str());
        if(flags!=INVALID_FILE_ATTRIBUTES){if(flags&FILE_ATTRIBUTE_REPARSE_POINT)return false;}
        else if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)return false;
    }
    return true;
}
std::wstring Lower(std::wstring value)
{std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});return value;}
bool Read(const fs::path& path,std::string& text)
{
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes))return false;
    text.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());return true;
}
bool Write(const fs::path& path,std::string_view text,bool replace)
{return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()},replace);}
bool Namespace(std::string_view value)
{
    if(value.empty())return true;
    for(std::size_t start=0;;)
    {
        const auto end=value.find('.',start);
        if(!EditorScriptTools::IsIdentifier(value.substr(start,end==std::string_view::npos?value.size()-start:end-start)))return false;
        if(end==std::string_view::npos)return true;start=end+1;
    }
}
bool FileName(std::string_view value)
{
    auto name=Lower(asset_io::FromUtf8(value).wstring());
    if(name==L"con"||name==L"prn"||name==L"aux"||name==L"nul"||name==L"clock$")return false;
    return !(name.size()==4&&(name.starts_with(L"com")||name.starts_with(L"lpt"))&&name[3]>=L'1'&&name[3]<=L'9');
}
Json ParseBuild(std::string_view text)
{
    bool duplicate=false;std::vector<std::set<std::string>> keys;
    auto json=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t event,Json& value)
    {
        if(depth>32)throw std::runtime_error("build document depth");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        else if(event==Json::parse_event_t::key&&!keys.empty()&&!keys.back().insert(value.get<std::string>()).second)duplicate=true;
        else if(event==Json::parse_event_t::object_end)keys.pop_back();return true;
    });
    if(duplicate||!json.is_object()||json.value("format",std::string{})!="SelfEngine.Build"||json.value("version",0)!=1)
        throw std::runtime_error("Project.InvalidBuildSettings");
    return json;
}
constexpr std::string_view ScriptProject=R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <Import Project="$(SelfEngineRoot)/SDK/SelfEngine.Scripts.props" Condition="Exists('$(SelfEngineRoot)/SDK/SelfEngine.Scripts.props')" />
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <PlatformTarget>x64</PlatformTarget>
    <AssemblyName>Game.Scripts</AssemblyName>
  </PropertyGroup>
  <ItemGroup Condition="Exists('$(SelfEngineRoot)/SDK/Managed/SelfEngine.Managed.dll')">
    <Reference Include="SelfEngine.Managed">
      <HintPath>$(SelfEngineRoot)/SDK/Managed/SelfEngine.Managed.dll</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
  <ItemGroup Condition="!Exists('$(SelfEngineRoot)/SDK/Managed/SelfEngine.Managed.dll') And Exists('$(SelfEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj')">
    <ProjectReference Include="$(SelfEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj" />
  </ItemGroup>
  <Target Name="CheckSelfEngineApi" BeforeTargets="ResolveReferences">
    <Error Condition="!Exists('$(SelfEngineRoot)/SDK/Managed/SelfEngine.Managed.dll') And !Exists('$(SelfEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj')" Text="Script.EngineApiMissing: SelfEngineRoot" />
  </Target>
</Project>
)xml";
}

bool EditorScriptTools::IsIdentifier(std::string_view value)
{
    static const std::set<std::string_view> keywords={"abstract","as","base","bool","break","byte","case","catch","char","checked","class","const","continue","decimal","default","delegate","do","double","else","enum","event","explicit","extern","false","finally","fixed","float","for","foreach","goto","if","implicit","in","int","interface","internal","is","lock","long","namespace","new","null","object","operator","out","override","params","private","protected","public","readonly","ref","return","sbyte","sealed","short","sizeof","stackalloc","static","string","struct","switch","this","throw","true","try","typeof","uint","ulong","unchecked","unsafe","ushort","using","virtual","void","volatile","while"};
    if(value.empty()||value.size()>128||keywords.contains(value)||value=="__arglist"||value=="__makeref"||value=="__reftype"||value=="__refvalue")return false;
    std::u32string decoded;if(!DecodeUtf8(value,decoded)||decoded.empty())return false;
    for(std::size_t index=0;index<decoded.size();++index)
    {
        const auto code=decoded[index];if(code==U'_')continue;
        if(code<128)
        {
            if((code>=U'a'&&code<=U'z')||(code>=U'A'&&code<=U'Z')||(index>0&&code>=U'0'&&code<=U'9'))continue;
            return false;
        }
        if(code>0xffff||code<32)return false;
        const auto character=static_cast<wchar_t>(code);WORD category=0,marks=0;
        if(!GetStringTypeW(CT_CTYPE1,&character,1,&category)||!GetStringTypeW(CT_CTYPE3,&character,1,&marks))return false;
        if((category&C1_ALPHA)&&!(marks&C3_NONSPACING))continue;
        if(index>0&&((category&C1_DIGIT)||(marks&C3_NONSPACING)))continue;
        return false;
    }
    return true;
}

bool EditorScriptTools::CreateBehaviour(const EditorScriptCreateRequest& request,EditorScriptCreateResult& result,
    std::string& errorKey,std::string& detail)
{
    result={};errorKey.clear();detail.clear();
    if(!IsIdentifier(request.className)||!Namespace(request.nameSpace)||!FileName(request.className))return Fail(errorKey,detail,"Script.TemplateIdentifierInvalid");
    struct Created {fs::path path;std::string text;};
    std::vector<Created> created;std::vector<fs::path> directories;fs::path root,buildPath;
    bool buildUpdated=false;std::string buildOriginal,buildReplacement;
    const auto rollback=[&]()
    {
        bool complete=true;
        if(buildUpdated)
        {
            std::string current;
            if(!Read(buildPath,current)||current!=buildReplacement||!Write(buildPath,buildOriginal,true))complete=false;
        }
        for(auto it=created.rbegin();it!=created.rend();++it)
        {
            std::string current;std::error_code error;
            if(!fs::exists(it->path,error)&&!error)continue;
            if(!Safe(root,it->path)||!Read(it->path,current)||current!=it->text||!fs::remove(it->path,error)||error)complete=false;
        }
        for(auto it=directories.rbegin();it!=directories.rend();++it)
        {
            // 直接尝试非递归删除：只删除本事务创建的空目录，避免预先枚举与删除之间的竞态。
            // RemoveDirectoryW 也不会误删被其他程序替换成普通文件的路径。
            if(!Safe(root,*it)){complete=false;detail=asset_io::Utf8(*it);continue;}
            if(!RemoveDirectoryW(it->c_str()))
            {
                const auto error=GetLastError();
                if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
                {complete=false;detail=asset_io::Utf8(*it);}
            }
        }
        if(!complete)errorKey="Script.TemplateRollbackIncomplete";result={};return false;
    };
    try
    {
        root=fs::absolute(request.projectRoot).lexically_normal();
        if(!Safe(root,root))return Fail(errorKey,detail,"Script.TemplatePathInvalid",root);
        ProjectDescription project;std::vector<ProjectDiagnostic> projectErrors;
        if(!Safe(root,root/"project.json")||!ReadProject(root/"project.json",project,projectErrors))return Fail(errorKey,detail,"Script.TemplateProjectInvalid",root/"project.json");
        buildPath=root/"build.json";
        if(!Safe(root,buildPath))return Fail(errorKey,detail,"Script.TemplatePathInvalid",buildPath);
        const bool hadBuild=fs::exists(buildPath);
        Json build={{"format","SelfEngine.Build"},{"version",1}};
        if(hadBuild){if(!Read(buildPath,buildOriginal))return Fail(errorKey,detail,"Script.TemplateProjectInvalid",buildPath);build=ParseBuild(buildOriginal);}
        const auto configured=build.value("scriptsProject",std::string{});const bool newProject=configured.empty();
        const auto scripts=root/"Scripts";
        fs::path projectPath=newProject?scripts/"Game.Scripts.csproj":fs::absolute(root/asset_io::FromUtf8(configured)).lexically_normal();
        if(!Safe(root,projectPath)||projectPath.extension()!=L".csproj"||(!newProject&&!fs::is_regular_file(projectPath)))
            return Fail(errorKey,detail,"Script.TemplateProjectInvalid",projectPath);
        if(!asset_io::Within(scripts,projectPath.parent_path())&&!asset_io::Within(projectPath.parent_path(),scripts))
            return Fail(errorKey,detail,"Script.TemplateProjectInvalid",projectPath);
        if(newProject&&fs::exists(projectPath))return Fail(errorKey,detail,"Script.TemplateConflict",projectPath);
        const auto sourceDirectory=asset_io::Within(scripts,projectPath.parent_path())?projectPath.parent_path():scripts;
        const auto source=sourceDirectory/asset_io::FromUtf8(request.className+".cs");
        if(!Safe(root,source)||!asset_io::Within(scripts,source))return Fail(errorKey,detail,"Script.TemplatePathInvalid",source);
        if(fs::exists(source))return Fail(errorKey,detail,"Script.TemplateConflict",source);
        const auto token=asset_io::NewId();if(token.empty())return Fail(errorKey,detail,"Assets.IdCreationFailed");
        const auto id="game."+token;
        std::string text="using SelfEngine;\n\n";
        if(!request.nameSpace.empty())text+="namespace "+request.nameSpace+";\n\n";
        text+="[ScriptId(\""+id+"\")]\npublic sealed class "+request.className+" : Behaviour\n{\n    public override void OnCreate()\n    {\n    }\n\n    public override void OnUpdate(double deltaSeconds)\n    {\n    }\n}\n";
        const auto create=[&](const fs::path& path,std::string contents)
        {
            if(!Safe(root,path))return false;
            std::vector<fs::path> missing;
            for(auto directory=path.parent_path();!fs::exists(directory);directory=directory.parent_path())
            {if(!Safe(root,directory))return false;missing.push_back(directory);}
            for(auto directory=missing.rbegin();directory!=missing.rend();++directory)
                if(fs::create_directory(*directory))directories.push_back(*directory);
            created.push_back({path,std::move(contents)});
            if(!Write(path,created.back().text,false)){created.pop_back();return false;}return true;
        };
        if(!create(source,text)){Fail(errorKey,detail,"Script.TemplateWriteFailed",source);return rollback();}
        if(newProject)
        {
            if(!create(projectPath,std::string(ScriptProject))){Fail(errorKey,detail,"Script.TemplateWriteFailed",projectPath);return rollback();}
            const auto props=scripts/"Directory.Build.props";
            if(!Safe(root,props)){Fail(errorKey,detail,"Script.TemplatePathInvalid",props);return rollback();}
            if(!fs::exists(props)&&!create(props,"<Project />\n")){Fail(errorKey,detail,"Script.TemplateWriteFailed",props);return rollback();}
            build["scriptsProject"]=asset_io::Utf8(projectPath.lexically_relative(root));
            if(build.value("scriptType",std::string{}).empty())build["scriptType"]=id;
            buildReplacement=build.dump(2)+"\n";
            if(hadBuild)
            {
                std::string current;if(!Read(buildPath,current)||current!=buildOriginal)
                {Fail(errorKey,detail,"Script.TemplateSourceChanged",buildPath);return rollback();}
                if(!Write(buildPath,buildReplacement,true)){Fail(errorKey,detail,"Script.TemplateWriteFailed",buildPath);return rollback();}
                buildUpdated=true;
            }
            else if(!create(buildPath,buildReplacement)){Fail(errorKey,detail,"Script.TemplateWriteFailed",buildPath);return rollback();}
        }
        EditorScriptCreateResult completed{source,projectPath,id};
        for(const auto& file:created)completed.created.push_back(file.path);
        if(buildUpdated)completed.updated.push_back(buildPath);
        result=std::move(completed);return true;
    }
    catch(const std::exception& failure){errorKey="Script.TemplateProjectInvalid";detail=failure.what();return rollback();}
}

bool EditorScriptTools::PrepareOpenSource(const fs::path& requestedRoot,const fs::path& requestedFile,
    std::uint32_t line,std::uint32_t column,const CodeEditorSettings& settings,EditorCodeOpenCommand& command,
    std::string& errorKey,std::string& detail)
{
    command={};errorKey.clear();detail.clear();
    try
    {
        const auto root=fs::absolute(requestedRoot).lexically_normal();
        const auto file=fs::absolute(requestedFile.is_absolute()?requestedFile:root/requestedFile).lexically_normal();
        static const std::set<std::wstring> extensions={L".cs",L".csproj",L".props",L".targets",L".json",L".hlsl",L".hlsli",L".txt",L".log"};
        if(!Safe(root,file)||!fs::is_regular_file(file)||!extensions.contains(Lower(file.extension().wstring())))
            return Fail(errorKey,detail,"Script.EditorFileInvalid",file);
        EditorCodeOpenCommand prepared;prepared.file=file;prepared.workingDirectory=root;
        if(settings.executable.empty()){prepared.useAssociation=true;command=std::move(prepared);return true;}
        const auto executable=fs::absolute(settings.executable).lexically_normal();const auto name=Lower(executable.filename().wstring());
        static const std::set<std::wstring> shells={L"cmd.exe",L"powershell.exe",L"pwsh.exe",L"wscript.exe",L"cscript.exe",L"mshta.exe",L"rundll32.exe"};
        if(!settings.executable.is_absolute()||!fs::is_regular_file(executable)||Lower(executable.extension().wstring())!=L".exe"||shells.contains(name))
            return Fail(errorKey,detail,"Script.EditorExecutableInvalid",settings.executable);
        prepared.executable=executable;prepared.arguments=settings.arguments;
        if(prepared.arguments.empty())prepared.arguments=name==L"code.exe"?std::vector<std::wstring>{L"--goto",L"{file}:{line}:{column}"}:std::vector<std::wstring>{L"{file}"};
        if(prepared.arguments.size()>64)return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
        bool fileIncluded=false;
        for(auto& argument:prepared.arguments)
        {
            if(argument.size()>16384||argument.find_first_of(L"\r\n")!=std::wstring::npos||argument.find(L'\0')!=std::wstring::npos)return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
            std::wstring expanded;
            for(std::size_t i=0;i<argument.size();)
            {
                if(argument[i]!=L'{'){expanded+=argument[i++];continue;}
                const auto close=argument.find(L'}',i);if(close==std::wstring::npos)return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
                const auto placeholder=argument.substr(i,close-i+1);
                if(placeholder==L"{file}"){expanded+=file.wstring();fileIncluded=true;}
                else if(placeholder==L"{line}")expanded+=std::to_wstring((std::max)(line,1u));
                else if(placeholder==L"{column}")expanded+=std::to_wstring((std::max)(column,1u));
                else return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
                i=close+1;
            }
            argument=std::move(expanded);
        }
        if(!fileIncluded||BuildProcessCommandLine(executable.wstring(),prepared.arguments).size()>32760)return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
        command=std::move(prepared);return true;
    }
    catch(const std::exception& failure){errorKey="Script.EditorFileInvalid";detail=failure.what();return false;}
}

bool EditorScriptTools::OpenSource(const fs::path& projectRoot,const fs::path& file,std::uint32_t line,std::uint32_t column,
    const CodeEditorSettings& settings,std::string& errorKey,std::string& detail)
{
    EditorCodeOpenCommand command;if(!PrepareOpenSource(projectRoot,file,line,column,settings,command,errorKey,detail))return false;
    if(command.useAssociation)
    {
        return StartEditorAssociatedFile(command.file,command.workingDirectory,errorKey);
    }
    return StartEditorProcess(command.executable.wstring(),command.arguments,command.workingDirectory,errorKey,true);
}
}
