#include "ToolLayer/Editor/EditorScriptTools.h"
#include "ToolLayer/Editor/EditorProcessRunner.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
using Json=nlohmann::json;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,const std::string& text)
{fs::create_directories(path.parent_path());std::ofstream stream(path,std::ios::binary|std::ios::trunc);stream<<text;Check(bool(stream),"fixture write");}
std::string Read(const fs::path& path)
{std::ifstream stream(path,std::ios::binary);Check(bool(stream),"fixture read");return {std::istreambuf_iterator<char>(stream),{}};}
void Project(const fs::path& root)
{
    fs::create_directories(root/"Content");fs::create_directories(root/"Scenes");
    fs::copy_file("Resources/Templates/BlankScene/project.json",root/"project.json");
    fs::copy_file("Resources/Templates/BlankScene/Scenes/Main.scene.json",root/"Scenes"/"Main.scene.json");
}
struct ReadLock
{
    HANDLE handle;
    explicit ReadLock(const fs::path& path):handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr))
    {Check(handle!=INVALID_HANDLE_VALUE,"lock existing build settings");}
    ~ReadLock(){CloseHandle(handle);}
};
void Templates(const fs::path& output)
{
    const auto root=output/L"新项目 with spaces";Project(root);
    EditorScriptCreateResult created;std::string key,detail;
    Check(EditorScriptTools::CreateBehaviour({root,"Spin","Game.Sample"},created,key,detail),"create first behaviour and script project");
    Check(created.created.size()==4&&created.updated.empty()&&created.source==root/"Scripts"/"Spin.cs"&&created.project==root/"Scripts"/"Game.Scripts.csproj","creation reports precise new files");
    Check(created.scriptId.starts_with("game.")&&created.scriptId.size()==37,"stable explicit random script ID");
    const auto source=Read(created.source),project=Read(created.project),build=Read(root/"build.json"),props=Read(root/"Scripts"/"Directory.Build.props");
    Check(source.find("[ScriptId(\""+created.scriptId+"\")]")!=std::string::npos&&source.find("public sealed class Spin : Behaviour")!=std::string::npos
        &&source.find("namespace Game.Sample;")!=std::string::npos,"template uses public engine behaviour contract");
    Check(project.find("SDK/Managed/SelfEngine.Managed.dll")!=std::string::npos&&project.find("Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj")!=std::string::npos
        &&project.find("net10.0")!=std::string::npos&&project.find(asset_io::Utf8(root))==std::string::npos,"same portable project supports source and package API without machine paths");
    const auto settings=Json::parse(build);Check(settings["scriptsProject"]=="Scripts/Game.Scripts.csproj"&&settings["scriptType"]==created.scriptId,"first behaviour configures build entry");
    const auto firstId=created.scriptId;
    EditorScriptCreateResult second;
    Check(EditorScriptTools::CreateBehaviour({root,"Mover","Game.Sample"},second,key,detail),"add behaviour to configured project");
    Check(second.created.size()==1&&second.updated.empty()&&second.project==created.project&&second.scriptId!=firstId,"configured project only adds source");
    Check(Read(created.source)==source&&Read(created.project)==project&&Read(root/"build.json")==build&&Read(root/"Scripts"/"Directory.Build.props")==props,"existing code project settings and props remain byte-identical");
    const auto secondSource=Read(second.source);
    Check(!EditorScriptTools::CreateBehaviour({root,"Mover","Game.Sample"},second,key,detail)&&key=="Script.TemplateConflict","duplicate filename never overwrites");
    Check(Read(root/"Scripts"/"Mover.cs")==secondSource&&Read(root/"build.json")==build,"duplicate creation leaves original identity intact");
    std::cout<<"Generated script project for managed build validation: "<<asset_io::Utf8(created.project)<<'\n';
}
void ConfigurationAndRollback(const fs::path& output)
{
    const auto root=output/"existing-build";Project(root);
    const Json original={{"format","SelfEngine.Build"},{"version",1},{"entryUi","UI/Menu.ui.json"},{"userExtension",{{"keep",17}}}};
    Put(root/"build.json",original.dump(4));EditorScriptCreateResult result;std::string key,detail;
    Check(EditorScriptTools::CreateBehaviour({root,"Begin","Game"},result,key,detail)&&result.created.size()==3&&result.updated==std::vector<fs::path>{root/"build.json"},"first script extends existing build settings in one transaction");
    auto updated=Json::parse(Read(root/"build.json"));updated.erase("scriptsProject");updated.erase("scriptType");
    Check(updated==original,"unrelated build settings preserved");

    const auto failed=output/"rollback";Project(failed);const auto bytes=original.dump(2);Put(failed/"build.json",bytes);
    {ReadLock lock(failed/"build.json");Check(!EditorScriptTools::CreateBehaviour({failed,"Retry","Game"},result,key,detail),"locked final build commit fails");}
    Check(result.created.empty()&&result.updated.empty()&&!fs::exists(failed/"Scripts")&&Read(failed/"build.json")==bytes,"failed setup rolls back all new files and keeps existing build settings");
    Check(key=="Script.TemplateWriteFailed"&&detail==asset_io::Utf8(failed/"build.json"),"complete rollback retains the original final commit failure");
    Check(EditorScriptTools::CreateBehaviour({failed,"Retry","Game"},result,key,detail),"same name can be retried after rollback");

    const auto existing=output/"rollback-existing-directory";Project(existing);Put(existing/"build.json",bytes);
    fs::create_directory(existing/"Scripts");
    {ReadLock lock(existing/"build.json");Check(!EditorScriptTools::CreateBehaviour({existing,"Retry","Game"},result,key,detail),"locked commit also fails with existing Scripts directory");}
    Check(key=="Script.TemplateWriteFailed"&&result.created.empty()&&result.updated.empty()
        &&fs::is_directory(existing/"Scripts")&&fs::is_empty(existing/"Scripts")&&Read(existing/"build.json")==bytes,
        "rollback keeps an existing empty Scripts directory and original settings");

    const auto collision=output/"project-collision";Project(collision);Put(collision/"Scripts"/"Game.Scripts.csproj","user owned project");
    Check(!EditorScriptTools::CreateBehaviour({collision,"First","Game"},result,key,detail)&&key=="Script.TemplateConflict","unconfigured existing project is not overwritten");
    Check(Read(collision/"Scripts"/"Game.Scripts.csproj")=="user owned project"&&!fs::exists(collision/"Scripts"/"First.cs")&&!fs::exists(collision/"build.json"),"project collision leaves no partial setup");
    const auto invalid=output/"invalid-build";Project(invalid);Put(invalid/"build.json","{broken");
    Check(!EditorScriptTools::CreateBehaviour({invalid,"First","Game"},result,key,detail)&&!fs::exists(invalid/"Scripts")&&Read(invalid/"build.json")=="{broken","malformed settings retained");
    Put(invalid/"build.json",Json{{"format","SelfEngine.Build"},{"version",1},{"scriptsProject","../outside.csproj"}}.dump());
    Check(!EditorScriptTools::CreateBehaviour({invalid,"First","Game"},result,key,detail)&&!fs::exists(invalid/"Scripts"),"outside configured project refused");
}
void Identifiers(const fs::path& output)
{
    Check(EditorScriptTools::IsIdentifier("Spin_2")&&EditorScriptTools::IsIdentifier("_Spin")&&EditorScriptTools::IsIdentifier("旋转"),"ASCII and BMP letter identifiers supported");
    for(const auto* name:{"","class","1Spin","../Spin","Spin;run","Spin Name","@class","Spin\nOther"})
        Check(!EditorScriptTools::IsIdentifier(name),"invalid or reserved identifier rejected");
    const auto root=output/"identifiers";Project(root);EditorScriptCreateResult result;std::string key,detail;
    Check(!EditorScriptTools::CreateBehaviour({root,"Spin","Game;Injected"},result,key,detail)&&key=="Script.TemplateIdentifierInvalid","namespace injection rejected");
    Check(!EditorScriptTools::CreateBehaviour({root,"NUL","Game"},result,key,detail)&&!fs::exists(root/"Scripts"),"reserved Windows filename rejected before writes");
    Check(EditorScriptTools::CreateBehaviour({root,"旋转","游戏"},result,key,detail),"Chinese identifiers generate UTF-8 source");
    Check(Read(result.source).find("namespace 游戏;")!=std::string::npos,"Chinese namespace preserved");
}
void EditorArguments(const fs::path& output)
{
    const auto root=output/L"代码项目";Project(root);const auto source=root/"Scripts"/L"文件 with spaces {literal}.cs";Put(source,"using SelfEngine;\n");
    EditorCodeOpenCommand command;std::string key,detail;
    Check(EditorScriptTools::PrepareOpenSource(root,source,10,3,{},command,key,detail)&&command.useAssociation&&command.file==source,"default association only receives selected source");
    const auto code=output/"fake editors"/"Code.exe";Put(code,"argument preparation fixture; never launched");
    CodeEditorSettings settings{code,{}};
    Check(EditorScriptTools::PrepareOpenSource(root,source,10,3,settings,command,key,detail)&&!command.useAssociation
        &&command.arguments==std::vector<std::wstring>{L"--goto",source.wstring()+L":10:3"},"VSCode stable goto parameters");
    settings.arguments={L"--file",L"{file}",L"--line",L"{line}",L"--column",L"{column}",L"literal & not a shell"};
    Check(EditorScriptTools::PrepareOpenSource(root,source,0,0,settings,command,key,detail)&&command.arguments[1]==source.wstring()
        &&command.arguments[3]==L"1"&&command.arguments[5]==L"1"&&command.arguments[6]==L"literal & not a shell","custom parameters remain independent arguments");
    const auto quoted=BuildProcessCommandLine(command.executable.wstring(),command.arguments);
    Check(quoted.find(L"\""+source.wstring()+L"\"")!=std::wstring::npos,"spaces and braces are safely passed to CreateProcess quoting");
    settings.arguments={L"{unknown}"};Check(!EditorScriptTools::PrepareOpenSource(root,source,1,1,settings,command,key,detail),"unknown placeholder rejected");
    settings.arguments={L"--version"};Check(!EditorScriptTools::PrepareOpenSource(root,source,1,1,settings,command,key,detail),"configured editor must receive chosen file");
    settings={fs::path("Code.exe"),{}};Check(!EditorScriptTools::PrepareOpenSource(root,source,1,1,settings,command,key,detail),"relative executable lookup rejected");
    const auto shell=output/"fake editors"/"cmd.exe";Put(shell,"never launched");settings={shell,{L"{file}"}};
    Check(!EditorScriptTools::PrepareOpenSource(root,source,1,1,settings,command,key,detail)&&key=="Script.EditorExecutableInvalid","shell cannot stand in for source editor");
    Put(root/"Scripts"/"run.cmd","exit 9");Check(!EditorScriptTools::PrepareOpenSource(root,root/"Scripts"/"run.cmd",1,1,{},command,key,detail),"default association never executes command scripts");
    Put(output/"outside.cs","class Outside {}\n");Check(!EditorScriptTools::PrepareOpenSource(root,output/"outside.cs",1,1,{},command,key,detail),"unselected project boundary enforced");
    Check(!EditorScriptTools::PrepareOpenSource(root,root/"missing.cs",1,1,{},command,key,detail),"missing diagnostic file returns failure");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: EditorScriptToolsTests <output-dir>");const auto id=asset_io::NewId();Check(!id.empty(),"output ID");
        const auto output=fs::absolute(argv[1])/("script-tools-"+id);fs::create_directories(output);
        Templates(output);ConfigurationAndRollback(output);Identifiers(output);EditorArguments(output);
        for(const auto& file:fs::recursive_directory_iterator(output))Check(file.path().filename().wstring().find(L".tmp-")==std::wstring::npos,"no temporary files left after transactions");
        std::cout<<"EditorScriptToolsTests passed (templates, stable IDs, project setup rollback, identifiers, safe editor parameters).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EditorScriptToolsTests failed: "<<error.what()<<'\n';return 1;}
}
