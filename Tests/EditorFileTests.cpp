#include "ToolLayer/Editor/SceneFileWorkflow.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
std::string Read(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
struct Attributes
{
    std::filesystem::path path;DWORD previous;
    explicit Attributes(std::filesystem::path value):path(std::move(value)),previous(GetFileAttributesW(path.c_str())){}
    ~Attributes(){if(previous!=INVALID_FILE_ATTRIBUTES)SetFileAttributesW(path.c_str(),previous);}
};
void Test(const std::filesystem::path& root)
{
    EditorSession editor;SceneFileWorkflow files(root);const auto entity=editor.Create("original");
    Check(files.SaveCurrent(editor,"Scenes/current.scene.json")==FileActionResult::Done&&!editor.Dirty(),"explicit current save creates chosen directory and file");
    const auto current=editor.Path();const auto saved=Read(current);
    EditorSession other;const auto otherId=other.Create("other");const auto otherPath=root/"Scenes/other.scene.json";
    Check(other.Save(otherPath),"other scene fixture");
    Check(editor.Rename(entity,"changed")&&files.RequestOpen(editor,otherPath)==FileActionResult::Waiting&&files.NeedsSaveDecision(),"dirty open waits for choice");
    Check(files.DecideUnsaved(editor,UnsavedChoice::Cancel,{})==FileActionResult::Done&&editor.Dirty()&&editor.Path()==current&&Read(current)==saved,"cancel open preserves dirty scene and saved file");
    Check(files.RequestOpen(editor,otherPath)==FileActionResult::Waiting&&files.DecideUnsaved(editor,UnsavedChoice::Save,{})==FileActionResult::Done,"save before open");
    Check(editor.Path()==otherPath&&editor.EditWorld().FindByStableId(otherId)&&Read(current).find("changed")!=std::string::npos,"saved original then opened requested scene");
    const auto openedScene=editor.EditScenes().ActiveScene();const auto openedInfo=editor.EditScenes().Inspect(openedScene);Check(openedInfo.active&&openedInfo.members.size()==1&&openedInfo.scope!=0,"editor open uses controlled scene activation and membership");
    const auto kept=root/"Scenes/keep.scene.json";{std::ofstream file(kept);file<<"keep existing bytes";}
    Check(editor.Rename(otherId,"dirty other")&&files.SaveAs(editor,kept)==FileActionResult::Waiting&&files.NeedsOverwriteDecision(),"SaveAs requires overwrite decision");
    Check(files.DecideOverwrite(editor,false)==FileActionResult::Done&&Read(kept)=="keep existing bytes"&&editor.Path()==otherPath,"decline overwrite preserves target and active path");
    Check(files.SaveAs(editor,kept)==FileActionResult::Waiting&&files.DecideOverwrite(editor,true)==FileActionResult::Done&&editor.Path()==kept&&!editor.Dirty(),"approved SaveAs commits target and path");
    const auto validBytes=Read(kept);
    {
        Attributes attributes(kept);Check(SetFileAttributesW(kept.c_str(),attributes.previous|FILE_ATTRIBUTE_READONLY)!=FALSE,"read-only scene fixture");
        Check(editor.Rename(otherId,"unsaved failure")&&files.SaveCurrent(editor,{})==FileActionResult::Failed&&editor.Path()==kept&&editor.Dirty()&&Read(kept)==validBytes,"write failure preserves source bytes, path and dirty world");
    }
    const auto obstruction=root/"directory-is-file";{std::ofstream file(obstruction);file<<"file";}
    Check(files.SaveAs(editor,obstruction/"bad.scene.json")==FileActionResult::Failed&&editor.Path()==kept&&Read(kept)==validBytes,"invalid directory does not change active file");
    EditorSession untitled;untitled.Create("untitled");SceneFileWorkflow untitledFiles(root);const auto otherBytes=Read(otherPath);
    Check(untitledFiles.RequestOpen(untitled,otherPath)==FileActionResult::Waiting&&untitledFiles.DecideUnsaved(untitled,UnsavedChoice::Save,otherPath)==FileActionResult::Failed&&untitledFiles.LastError()=="Editor.SavePathConflictsOpen"&&Read(otherPath)==otherBytes,"untitled save cannot destroy requested open target");
    Check(untitledFiles.DecideUnsaved(untitled,UnsavedChoice::Save,"Scenes/untitled.scene.json")==FileActionResult::Done&&Read(root/"Scenes/untitled.scene.json").find("untitled")!=std::string::npos&&untitled.Path()==otherPath,"choose different untitled path then continue open");
    const auto bad=root/"Scenes/broken.scene.json";{std::ofstream file(bad);file<<"{broken";}
    std::string before,error,after;Check(SerializeScene(untitled.EditWorld(),before,error),"snapshot before bad open");
    Check(untitledFiles.RequestOpen(untitled,bad)==FileActionResult::Failed&&!untitledFiles.Pending()&&SerializeScene(untitled.EditWorld(),after,error)&&before==after&&untitled.Path()==otherPath,"bad clean open preserves active scene");
    const auto keptScene=untitled.EditScenes().ActiveScene();const auto keptRevision=untitled.EditScenes().Inspect(keptScene).revision;const auto keptEntity=untitled.EditWorld().FindByStableId(otherId);
    const auto unsupported=root/"Scenes/unsupported.scene.json";const std::string unsupportedText="{\"format\":\"SelfEngine.Scene\",\"version\":999,\"entities\":[]}";{std::ofstream file(unsupported);file<<unsupportedText;}
    Check(!untitled.Open(unsupported,true)&&untitled.EditScenes().ActiveScene()==keptScene&&untitled.EditScenes().Inspect(keptScene).revision==keptRevision&&untitled.EditWorld().IsAlive(keptEntity)&&untitled.Path()==otherPath&&Read(unsupported)==unsupportedText,"failed editor candidate preserves runtime identity, active file and source bytes");
    Check(untitled.Rename(otherId,"close dirty")&&untitled.StartPlay(),"running dirty close fixture");
    Check(untitledFiles.RequestClose(untitled)==FileActionResult::Waiting&&untitled.IsPlaying()&&!untitledFiles.CloseApproved(),"dirty close keeps runtime until decision");
    Check(untitledFiles.DecideUnsaved(untitled,UnsavedChoice::Cancel,{})==FileActionResult::Done&&untitled.IsPlaying()&&!untitledFiles.CloseApproved(),"cancel close preserves runtime");
    Check(untitledFiles.RequestClose(untitled)==FileActionResult::Waiting&&untitledFiles.DecideUnsaved(untitled,UnsavedChoice::Save,{})==FileActionResult::Done&&untitledFiles.CloseApproved()&&!untitled.IsPlaying()&&!untitled.Dirty(),"save-close commits edit scene and stops runtime");
    Check(untitledFiles.RequestClose(untitled,true)==FileActionResult::Waiting&&untitledFiles.DecideUnsaved(untitled,UnsavedChoice::Cancel,{})==FileActionResult::Done&&!untitledFiles.CloseApproved(),"other document dirty participates in close guard");
    // 外部修改：保存前停下，取消不覆盖；另存与重新加载各自刷新基线。
    EditorSession external;SceneFileWorkflow externalFiles(root);external.Create("external");
    Check(externalFiles.SaveCurrent(external,"Scenes/external.scene.json")==FileActionResult::Done&&!external.Dirty(),"external fixture saved");
    const auto externalPath=external.Path();
    EditorSession writer;writer.Create("outside");Check(writer.Save(externalPath),"external writer saves over scene");
    const auto externalBytes=Read(externalPath);
    external.Create("dirty local");
    Check(externalFiles.SaveCurrent(external,{})==FileActionResult::Waiting&&externalFiles.NeedsExternalDecision(),"external change stops save");
    Check(Read(externalPath)==externalBytes,"external change is not overwritten");
    Check(externalFiles.DecideExternal(external,ExternalChoice::Cancel)==FileActionResult::Done&&!externalFiles.NeedsExternalDecision()&&external.Dirty(),"external cancel keeps local dirty state");
    Check(externalFiles.SaveCurrent(external,{})==FileActionResult::Waiting&&externalFiles.DecideExternal(external,ExternalChoice::SaveAs)==FileActionResult::Waiting&&externalFiles.TakeExternalSaveAsRequest(),"external save-as request");
    const auto externalNew=root/"Scenes/external-new.scene.json";
    Check(externalFiles.SaveAs(external,externalNew)==FileActionResult::Done&&external.Path()==externalNew&&!external.Dirty(),"external save-as commits new path");
    external.Create("dirty again");
    writer.Create("outside again");Check(writer.Save(externalNew),"second external writer");
    Check(externalFiles.SaveCurrent(external,{})==FileActionResult::Waiting&&externalFiles.NeedsExternalDecision(),"second external change stops save");
    Check(externalFiles.DecideExternal(external,ExternalChoice::Reload)==FileActionResult::Done&&external.Path()==externalNew&&!external.Dirty(),"external reload discards local revision");
    const auto atomic=root/"atomic.bin";const std::string first="first",second="second";
    Check(asset_io::WriteAtomic(atomic,{reinterpret_cast<const std::uint8_t*>(first.data()),first.size()},false),"atomic new write");
    Check(!asset_io::WriteAtomic(atomic,{reinterpret_cast<const std::uint8_t*>(second.data()),second.size()},false)&&Read(atomic)==first,"atomic no-replace protects concurrent file");
    World scene;scene.Create("collision");Check(!SaveScene(atomic,scene,error,false)&&Read(atomic)==first,"scene no-replace protects concurrent file");
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root))Check(entry.path().filename().string().find(".tmp")==std::string::npos,"failed writes leave no temporary file");
}
}
int main(int argc,char** argv)
{
    try{const auto root=std::filesystem::absolute(argc>1?argv[1]:"out/validation/editor-files")/asset_io::NewId();std::filesystem::create_directories(root);Test(root);std::cout<<"EditorFileTests passed (save/open/SaveAs, dirty decisions, close/runtime, IO failure and no-replace).\n";return 0;}
    catch(const std::exception& error){std::cerr<<"EditorFileTests failed: "<<error.what()<<'\n';return 1;}
}
