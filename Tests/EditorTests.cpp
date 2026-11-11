#include "ToolLayer/Editor/EditorSession.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <cmath>
#include <set>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL " << __LINE__ << ": " << #c << '\n'; return 1; } } while(false)
int main(int argc, char** argv)
{
    const auto directory = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("out/tests/editor");
    std::filesystem::create_directories(directory);
    se::EditorSession editor;
    auto parent = editor.Create("Parent"); auto child = editor.Create("Child",parent);
    CHECK(parent && child && editor.EditWorld().Size() == 2);
    const auto handle = editor.EditWorld().FindByStableId(child);
    se::Transform transform; transform.position = {1,2,3};
    CHECK(editor.BeginEdit("drag"));
    CHECK(editor.SetTransform(child,transform)); transform.position.x = 5;
    CHECK(editor.SetTransform(child,transform));
    CHECK(editor.CommitEdit() && editor.UndoCount() == 3);
    CHECK(!editor.EditWorld().IsAlive(handle));
    CHECK(editor.Undo());
    CHECK(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child))->position.x == 0);
    CHECK(editor.Redo());
    CHECK(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child))->position.x == 5);
    CHECK(!editor.SetParent(parent,child));
    auto duplicate = editor.Duplicate(parent);
    CHECK(duplicate && editor.EditWorld().Size() == 4);
    CHECK(editor.Delete(parent) && editor.EditWorld().Size() == 2);
    CHECK(editor.Undo() && editor.EditWorld().Size() == 4);
    CHECK(editor.Save(directory / "scene.json") && !editor.Dirty());
    CHECK(editor.StartPlay());
    CHECK(editor.PlayWorld()->Destroy(editor.PlayWorld()->FindByStableId(parent)));
    CHECK(editor.PlayWorld()->Size() == 2 && editor.EditWorld().Size() == 4);
    CHECK(!editor.Create("NoEditWhilePlaying"));
    editor.StopPlay();
    CHECK(editor.Rename(child,"Changed") && editor.Dirty());
    CHECK(!editor.Open(directory / "scene.json"));
    CHECK(editor.Open(directory / "scene.json",true));
    CHECK(!editor.Dirty());
    CHECK(editor.EditWorld().Get(editor.EditWorld().FindByStableId(child))->name == "Child");
    for (int i = 0; i < 30; ++i) { CHECK(editor.StartPlay()); editor.StopPlay(); }
    CHECK(editor.EditWorld().Size() == 4);
    {
        se::EditorSession lifecycle;const auto id=lifecycle.Create("edit remains");CHECK(lifecycle.Select(id));
        std::string baseline,after,error;CHECK(se::SerializeScene(lifecycle.EditWorld(),baseline,error));
        const auto history=lifecycle.UndoCount();int starts=0,stops=0,mode=0;bool valid=true;
        CHECK(lifecycle.SetPlayCallbacks([&](se::World& runtime){
            ++starts;const auto original=runtime.FindByStableId(id);
            if(mode==0){lifecycle.StopPlay();lifecycle.StopPlay();valid=valid&&lifecycle.IsPlaying()&&runtime.IsAlive(original);runtime.Create("after deferred stop");return true;}
            if(mode==1)return false;if(mode==2)throw std::runtime_error("expected startup failure");
            runtime.Destroy(original);runtime.Create("runtime only");return true;
        },[&](se::World& runtime){++stops;valid=valid&&lifecycle.PlayWorld()==&runtime&&runtime.Size()>0;lifecycle.StopPlay();valid=valid&&!lifecycle.StartPlay();if(mode==2)throw std::runtime_error("expected shutdown failure");}));
        for(int cycle=0;cycle<64;++cycle)
        {
            mode=cycle%4;const bool started=lifecycle.StartPlay();CHECK(started==(mode==3));
            if(mode==0)CHECK(lifecycle.LastError()=="Editor.PlayStartCancelled");
            if(mode==1||mode==2)CHECK(lifecycle.LastError()=="Editor.PlayStartFailed");
            if(started)lifecycle.StopPlay();lifecycle.StopPlay();
            CHECK(valid&&!lifecycle.IsPlaying()&&starts==cycle+1&&stops==starts);
            CHECK(se::SerializeScene(lifecycle.EditWorld(),after,error)&&after==baseline&&lifecycle.Selected()==id&&lifecycle.UndoCount()==history);
        }
    }
    se::EditorSession tree;
    CHECK(tree.Apply("fixture",[](se::World& world){
        const auto outside=world.Create("Outside",950),child=world.Create("Child",20),root=world.Create("Root",900),leaf=world.Create("Leaf",10),sibling=world.Create("Sibling",30);
        return outside&&child&&root&&leaf&&sibling&&world.SetParent(root,outside)&&world.SetParent(child,root)&&world.SetParent(leaf,child)&&world.SetParent(sibling,root)
            &&world.SetTransform(outside,{{-2,0,0},{},{1,1,1}})&&world.SetTransform(root,{{3,2,1},{},{2,1,3}})&&world.SetTransform(child,{{1,0,0},{},{1,1,1}})
            &&world.SetCamera(root,{.7f,.25f,150,false})&&world.SetMesh(child,{"shared mesh",{"shared material","second material"},false,true})
            &&world.SetLight(leaf,{{.5f,.4f,.3f},12,se::LightType::Point,7})&&world.SetEnabled(leaf,false);
    }));
    const auto hierarchy=se::BuildEditorHierarchy(tree.EditWorld());
    CHECK(hierarchy.size()==5&&hierarchy[0].id==950&&hierarchy[1].id==900&&hierarchy[2].id==20&&hierarchy[3].id==10&&hierarchy[4].id==30);
    CHECK(hierarchy[2].depth==2&&hierarchy[2].hasChildren&&hierarchy[3].depth==3&&!hierarchy[3].hasChildren);
    const auto originalHandle=tree.EditWorld().FindByStableId(20);const auto originalWorld=tree.EditWorld().GetWorldMatrix(originalHandle);
    const auto copyRoot=tree.Duplicate(900);CHECK(copyRoot&&copyRoot!=900&&tree.EditWorld().Size()==9&&tree.Selected()==copyRoot);
    se::StableEntityId copyChild=0,copyLeaf=0;
    for(const auto& item:se::BuildEditorHierarchy(tree.EditWorld()))
    {
        const auto* record=tree.EditWorld().Get(tree.EditWorld().FindByStableId(item.id));
        if(record->name=="Child Copy")copyChild=item.id;if(record->name=="Leaf Copy")copyLeaf=item.id;
    }
    CHECK(copyChild&&copyLeaf&&copyChild!=20&&copyLeaf!=10);
    const auto& copied=tree.EditWorld();const auto copiedChild=copied.FindByStableId(copyChild),copiedLeaf=copied.FindByStableId(copyLeaf);
    CHECK(copied.Get(copied.FindByStableId(copyRoot))->parent==copied.FindByStableId(950));
    CHECK(copied.Get(copiedChild)->parent==copied.FindByStableId(copyRoot)&&copied.Get(copiedLeaf)->parent==copiedChild&&!copied.Get(copiedLeaf)->enabled);
    CHECK(copied.GetMesh(copiedChild)->materials==std::vector<std::string>({"shared material","second material"})&&!copied.GetMesh(copiedChild)->castsShadow);
    CHECK(copied.GetCamera(copied.FindByStableId(copyRoot))->nearPlane==.25f&&copied.GetLight(copiedLeaf)->range==7);
    for(size_t index=0;index<16;++index)CHECK(std::abs(copied.GetWorldMatrix(copiedChild).values[index]-originalWorld.values[index])<.0001f);
    CHECK(tree.SetProperty(copyChild,"transform.position",se::Vec3{9,0,0})&&tree.EditWorld().GetTransform(tree.EditWorld().FindByStableId(20))->position.x==1);
    CHECK(tree.SetParent(20,0)&&tree.EditWorld().GetTransform(tree.EditWorld().FindByStableId(20))->position.x==1);
    CHECK(tree.Undo()&&tree.EditWorld().Get(tree.EditWorld().FindByStableId(20))->parent==tree.EditWorld().FindByStableId(900));
    std::string beforeCycle,afterCycle,error;CHECK(se::SerializeScene(tree.EditWorld(),beforeCycle,error));
    CHECK(!tree.SetParent(900,10)&&se::SerializeScene(tree.EditWorld(),afterCycle,error)&&afterCycle==beforeCycle);
    CHECK(tree.Select(20)&&tree.Delete(900)&&tree.Selected()==0&&tree.EditWorld().Size()==5);
    CHECK(!tree.EditWorld().ResolveReference(20)&&tree.EditWorld().LastError()=="World.UnresolvedReference"&&!tree.EditWorld().IsAlive(originalHandle));
    CHECK(tree.EditWorld().GetMesh(tree.EditWorld().FindByStableId(copyChild))&&tree.EditWorld().GetLight(tree.EditWorld().FindByStableId(copyLeaf)));
    CHECK(tree.Undo()&&tree.Selected()==20&&tree.EditWorld().ResolveReference(20)&&!tree.EditWorld().IsAlive(originalHandle));
    CHECK(tree.Redo()&&!tree.EditWorld().ResolveReference(20));
    CHECK(!tree.Apply("reuse",[](se::World& world){return bool(world.Create("unexpected",20));}));
    CHECK(tree.Save(directory/"hierarchy.scene.json"));
    se::EditorSession reopened;CHECK(reopened.Open(directory/"hierarchy.scene.json",true));
    CHECK(!reopened.Apply("reuse",[](se::World& world){return bool(world.Create("unexpected",20));}));
    const auto created=reopened.Create("new after reopen");CHECK(created&&created!=20&&created!=900);
    CHECK(reopened.Undo()&&!reopened.EditWorld().FindByStableId(created));
    CHECK(!reopened.Apply("undo reuse",[=](se::World& world){return bool(world.Create("unexpected",created));}));
    CHECK(reopened.StartPlay()&&!reopened.Duplicate(copyRoot)&&!reopened.Delete(copyRoot)&&!reopened.SetParent(copyRoot,0));reopened.StopPlay();
    se::World deep;se::EntityHandle last{};
    for(int index=255;index>=0;--index){auto current=deep.Create("deep",static_cast<se::StableEntityId>(10000+index));CHECK(current&&deep.SetParent(current,last));last=current;}
    const auto deepRows=se::BuildEditorHierarchy(deep);CHECK(deepRows.size()==256&&deepRows.front().id==10255&&deepRows.back().depth==255);
    // 同一文档无论运行槽位如何分配，持久字节和编辑脏状态均保持相同。
    se::World ordered,reversed;ordered.Create("first",1);ordered.Create("second",2);
    reversed.Create("second",2);reversed.Create("first",1);
    std::string orderedText,reversedText;
    CHECK(se::SerializeScene(ordered,orderedText,error)&&se::SerializeScene(reversed,reversedText,error)&&orderedText==reversedText);
    std::cout << "Editor transactions, hierarchy undo, stable selection, atomic scene and Play isolation passed\n";
    return 0;
}
