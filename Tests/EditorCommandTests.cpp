#include "ToolLayer/Editor/EditorCommand.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::trunc);output<<text;}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: EditorCommandTests <output-dir>");
        auto output=fs::absolute(fs::path(argv[1]));
        std::error_code cleanup;fs::remove_all(output,cleanup);fs::create_directories(output);
        EditorCommandRouter router;
        int executions=0;
        Check(router.Register("editor.save",[&](const EditorCommandContext&){++executions;return EditorCommandOutcome::Succeeded();}),"register command");
        Check(router.Register("editor.play",[&](const EditorCommandContext&){return EditorCommandOutcome::Succeeded();},
            [](std::string& reason){reason="Editor.Playing";return false;}),"register disabled command");
        Check(router.Count()==2&&router.Registered("editor.save")&&!router.Registered("editor.missing"),"command registry lookup");
        std::string reason;
        Check(router.Enabled("editor.save",reason)&&reason.empty(),"command enabled");
        Check(!router.Enabled("editor.play",reason)&&reason=="Editor.Playing","command disabled reason");
        const auto executed=router.Execute("editor.save",{});
        Check(executed.state==EditorCommandResult::Succeeded&&executions==1,"command executed once");
        const auto unknown=router.Execute("editor.missing",{});
        Check(unknown.state==EditorCommandResult::Rejected&&unknown.diagnosticKey=="Editor.CommandUnknown","unknown command rejected");
        const auto disabled=router.Execute("editor.play",{});
        Check(disabled.state==EditorCommandResult::Rejected&&disabled.diagnosticKey=="Editor.Playing","disabled command rejected with reason");
        router.SetWorkspaceGeneration(7);
        const auto stale=router.Execute("editor.save",{.workspaceGeneration=6});
        Check(stale.state==EditorCommandResult::Rejected&&stale.diagnosticKey=="Editor.CommandStaleWorkspace"&&executions==1,"stale workspace rejected");
        Check(router.Execute("editor.save",{.workspaceGeneration=7}).state==EditorCommandResult::Succeeded&&executions==2,"current workspace executes");

        EditorDocumentRegistry documents;
        const auto scenePath=output/"Scenes/Main.scene.json",materialPath=output/"Materials/Default.material.json";
        Put(scenePath,"scene");Put(materialPath,"material");
        std::string error;
        Check(documents.Register("scene","scene",scenePath,error)&&documents.Register("material","material",materialPath,error),"register documents");
        Check(documents.Size()==2&&!documents.Dirty("scene")&&documents.DirtyCount()==0,"clean documents");
        Check(documents.MarkEdited("scene")&&documents.MarkEdited("material")&&documents.DirtyCount()==2,"edits make documents dirty");
        Check(documents.DirtyDocuments()==std::vector<std::string>{"material","scene"},"dirty list sorted");
        Check(documents.MarkSaved("scene",error)&&!documents.Dirty("scene")&&documents.Dirty("material"),"single save clears only that document");
        Check(documents.MarkEdited("scene")&&documents.Dirty("scene"),"edit after save is dirty again");
        const auto savedRevision=documents.Find("scene")->savedRevision;
        Check(documents.RevertTo("scene",savedRevision)&&!documents.Dirty("scene"),"undo back to save point is clean");
        Check(documents.MarkEdited("scene")&&documents.MarkEdited("material"),"prepare save all");
        std::vector<std::string> saveOrder;
        const auto first=documents.SaveAll([&](const EditorDocumentEntry& entry,std::string& key,std::string& detail){
            saveOrder.push_back(entry.id);
            if(entry.id=="material"){key="Editor.SaveRejected";detail="material locked";return false;}
            return true;});
        Check(first.size()==2,"save all reports every dirty document");
        Check(saveOrder==std::vector<std::string>{"material","scene"},"resource documents are committed before scene references");
        bool sceneSaved=false,materialFailed=false;
        for(const auto& result:first)
        {
            if(result.id=="scene")sceneSaved=result.saved;
            if(result.id=="material")materialFailed=!result.saved&&result.diagnosticKey=="Editor.SaveRejected";
        }
        Check(sceneSaved&&materialFailed&&!documents.Dirty("scene")&&documents.Dirty("material"),"save all keeps failures dirty");
        Put(materialPath,"material changed externally");
        Check(documents.HasExternalChange("material"),"external change detected");
        const auto conflict=documents.SaveAll([](const EditorDocumentEntry&,std::string&,std::string&){return true;});
        Check(conflict.size()==1&&conflict[0].diagnosticKey=="Editor.ExternalChange"&&documents.Dirty("material"),"external change blocks save");
        Check(!documents.HasExternalChange("scene"),"unchanged document reports no conflict");
        Check(documents.AcceptExternal("material",error)&&!documents.HasExternalChange("material"),"accept external refreshes baseline");
        Put(materialPath,"material changed again");
        Check(documents.MarkReloaded("material",error)&&!documents.Dirty("material"),"reload discards edit revision");
        Check(documents.MarkEdited("material")&&documents.SaveAs("material",output/"Materials/Copy.material.json",error)==false,"save as to missing file reports error");
        Put(output/"Materials/Copy.material.json","copy");
        Check(documents.SaveAs("material",output/"Materials/Copy.material.json",error)&&!documents.Dirty("material")&&documents.Find("material")->path==output/"Materials/Copy.material.json","save as retargets document");
        // 尚未落盘的新文档：没有基线不能被误判为外部变更，保存回调创建文件后才标记干净。
        const auto newScene=output/"Scenes/New.scene.json";
        Check(documents.Register("new","scene",newScene,error)&&documents.MarkEdited("new"),"untitled document registers dirty");
        Check(!documents.HasExternalChange("new"),"missing baseline is not an external change");
        const auto created=documents.SaveAll([&](const EditorDocumentEntry& entry,std::string&,std::string&){
            if(entry.id!="new")return false;
            Put(entry.path,"new scene");return true;});
        Check(created.size()==1&&created[0].saved&&!documents.Dirty("new"),"new document saves after creating its file");
        std::cout<<"EditorCommandTests passed (command registry, enabling, generation guard, document revisions, save-all, new document and external change).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"EditorCommandTests failed: "<<failure.what()<<'\n';return 1;
    }
}
