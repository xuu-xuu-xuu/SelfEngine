#include "CoreLayer/Math/TransformDecomposition.h"
#include "ToolLayer/Editor/ImGuiEditor.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
bool Near(Vec3 a,Vec3 b,float epsilon=.001f){return Length(a-b)<epsilon;}
bool Near(const Mat4& a,const Mat4& b,float epsilon=.0002f)
{for(size_t index=0;index<16;++index)if(std::abs(a.values[index]-b.values[index])>epsilon*std::max(1.0f,std::abs(a.values[index])))return false;return true;}
std::uint32_t randomState=424242;
float Random(){randomState^=randomState<<13;randomState^=randomState>>17;randomState^=randomState<<5;return float(randomState&65535)/65535;}
std::string Logical(const World& world)
{std::string text,error;Check(SerializeScene(world,text,error),"logical snapshot");auto json=nlohmann::json::parse(text);json.erase("retiredIds");return json.dump();}

void Decomposition()
{
    for(int index=0;index<8192;++index)
    {
        const auto axis=Normalize(Vec3{Random()-.5f,Random()-.5f,Random()-.5f});
        Transform input{{1000*Random(),-50*Random(),20*Random()},AxisAngle(axis,(Random()-.5f)*2*Pi),{.1f+4*Random(),.1f+4*Random(),.1f+4*Random()}};
        if(index&1)input.scale.x=-input.scale.x;if(index&2)input.scale.y=-input.scale.y;if(index&4)input.scale.z=-input.scale.z;
        Transform output;Check(TryDecomposeTrs(input.Matrix(),output)&&Near(input.Matrix(),output.Matrix()),"dense TRS reconstruction with every mirror parity/quaternion branch");
        Check(output.scale.y>0&&output.scale.z>0&&std::abs(LengthSquared(output.rotation)-1)<1e-5f,"canonical signs and unit quaternion");
    }
    const Transform large{{},{},{1e20f,1e-5f,1e-3f}};Transform output;Check(TryDecomposeTrs(large.Matrix(),output)&&Near(large.Matrix(),output.Matrix()),"double length avoids float norm overflow");
    output.position={17,18,19};const auto before=output.Matrix();
    auto bad=Mat4{};bad(0,1)=.2f;Check(!TryDecomposeTrs(bad,output)&&output.Matrix().values==before.values,"shear rejected without output mutation");
    bad=Scaling({1,0,1});Check(!TryDecomposeTrs(bad,output),"singular TRS rejected");
    bad=Mat4{};bad(3,2)=.1f;Check(!TryDecomposeTrs(bad,output),"projective matrix rejected");
    bad=Mat4{};bad(0,0)=std::numeric_limits<float>::quiet_NaN();Check(!TryDecomposeTrs(bad,output),"nonfinite matrix rejected");
    Check(!TryDecomposeTrs(Mat4{},output,0),"invalid tolerance rejected");
    World snapshots;
    for(int index=0;index<64;++index)
    {const auto entity=snapshots.Create("quaternion snapshot");Check(snapshots.SetTransform(entity,{{},AxisAngle(Normalize(Vec3{Random(),Random(),Random()}),Random()*2*Pi),{1,1,1}}),"snapshot transform");}
    std::string expected,error;Check(SerializeScene(snapshots,expected,error),"initial precise snapshot");
    for(int round=0;round<8;++round)
    {
        World copied=snapshots;std::string text;Check(SerializeScene(copied,text,error)&&text==expected,"cloning preserves normalized float bytes");
        Check(DeserializeScene(text,snapshots,error)&&SerializeScene(snapshots,text,error)&&text==expected,"snapshot restoration does not renormalize unit floats repeatedly");
    }
}

void Spaces()
{
    EditorSession editor;StableEntityId parent=0,child=0,other=0;
    const Transform parentValue{{2,1,-3},AxisAngle({0,1,0},.6f),{2,2,2}};
    const Transform childValue{{1,2,3},AxisAngle({0,0,1},.3f),{1,1.5f,2}};
    Check(editor.Apply("fixture",[&](World& world){
        auto p=world.Create("parent"),c=world.Create("child"),o=world.Create("other");parent=world.Get(p)->stableId;child=world.Get(c)->stableId;other=world.Get(o)->stableId;
        return world.SetTransform(p,parentValue)&&world.SetTransform(c,childValue)&&world.SetParent(c,p)&&world.SetTransform(o,{{-3,2,1},AxisAngle({1,0,0},-.4f),{3,3,3}});
    }),"space fixture");
    Check(editor.Select(child),"select space object");EditorGizmo gizmo;
    const auto matrix=[&]{return editor.EditWorld().GetWorldMatrix(editor.EditWorld().FindByStableId(child));};
    const auto original=matrix();const auto count=editor.UndoCount();
    Check(gizmo.Begin(editor,GizmoMode::Translate,GizmoAxis::X,{},{1,0},80,GizmoSpace::World)&&gizmo.Update(editor,{40,0})&&gizmo.Update(editor,{80,0})&&gizmo.End(editor),"world translate under transformed parent");
    Check(Near(TransformPoint(matrix(),{}),TransformPoint(original,{})+Vec3{1,0,0})&&editor.UndoCount()==count+1,"world distance and drag merge");
    Check(editor.Undo()&&Near(matrix(),original),"world translation undo");
    const auto localAxis=Rotate(parentValue.rotation*childValue.rotation,{1,0,0});
    Check(gizmo.Begin(editor,GizmoMode::Translate,GizmoAxis::X,{},{1,0},80,GizmoSpace::Local)&&gizmo.Update(editor,{80,0})&&gizmo.End(editor),"local translate with shared displayed orientation");
    Check(Near(TransformPoint(matrix(),{}),TransformPoint(original,{})+localAxis),"local direction ignores parent scale while inverse maps full world delta");
    Check(editor.Undo(),"undo local translate");
    Check(gizmo.Begin(editor,GizmoMode::Rotate,GizmoAxis::Y,{},{1,0},80,GizmoSpace::World)&&gizmo.Update(editor,{24,0})&&gizmo.End(editor),"world rotate under uniform scaled rotated parent");
    auto expected=Rotation(AxisAngle({0,1,0},.3f))*original;for(size_t row=0;row<3;++row)expected(row,3)=original(row,3);
    Check(Near(matrix(),expected)&&editor.Undo(),"world rotation matches independent matrix oracle and undo");
    Check(gizmo.Begin(editor,GizmoMode::Scale,GizmoAxis::Y,{},{0,1},80,GizmoSpace::Local)&&gizmo.Update(editor,{0,40})&&gizmo.End(editor),"local scale");
    auto scaled=childValue;scaled.scale.y*=1.5f;Check(Near(matrix(),parentValue.Matrix()*scaled.Matrix())&&editor.Undo(),"local scale matrix oracle");
    Check(editor.SetParent(child,other,ReparentMode::KeepWorld)&&Near(matrix(),original),"reparent preserves entire world matrix");
    Check(editor.Undo()&&editor.EditWorld().Get(editor.EditWorld().FindByStableId(child))->parent==editor.EditWorld().FindByStableId(parent)&&Near(matrix(),original),"reparent undo restores hierarchy and transform");
    Check(editor.SetProperty(parent,"transform.scale",Vec3{2,1,3}),"nonuniform parent fixture");
    const auto nonuniform=matrix();
    Check(gizmo.Begin(editor,GizmoMode::Rotate,GizmoAxis::X,{},{1,0},80,GizmoSpace::World)&&!gizmo.Update(editor,{32,0})&&gizmo.LastError()=="Editor.TransformNotRepresentable"&&gizmo.End(editor,false),"unrepresentable world shear fails and cancels");
    Check(Near(matrix(),nonuniform),"failed world operation leaves original matrix");
    Check(!editor.SetParent(child,0,ReparentMode::KeepWorld)&&editor.LastError()=="Editor.TransformNotRepresentable"&&Near(matrix(),nonuniform),"unrepresentable world reparent is atomic");
    Check(gizmo.Begin(editor,GizmoMode::Translate,GizmoAxis::Z,{},{1,0},80,GizmoSpace::World)&&gizmo.Update(editor,{80,0})&&gizmo.End(editor,false)&&Near(matrix(),nonuniform),"cancel valid drag under nonuniform parent");
    Check(editor.SetProperty(parent,"transform.scale",Vec3{0,1,1})&&!gizmo.Begin(editor,GizmoMode::Translate,GizmoAxis::X,{},{1,0}),"singular parent does not begin edit transaction");
    Check(editor.SetProperty(parent,"transform.scale",Vec3{1,1,1})&&gizmo.Begin(editor,GizmoMode::Translate,GizmoAxis::X,{},{1,0})&&gizmo.End(editor,false),"gizmo recovers after invalid input");

    EditorSession rayEditor;const auto id=rayEditor.Create("ray drag");Check(rayEditor.Select(id),"ray selection");
    CameraSnapshot camera;CameraError error;const FreeCameraState cameraState{{3,4,8},.2f,-.25f};
    Check(BuildCameraSnapshot({1,FreeCameraTransform(cameraState).Matrix(),{},true},{11,17,321,211},1,camera,error),"drag camera");
    const Vec3 origin{0,0,0};const auto start=ProjectWorldPoint(camera,origin),end=ProjectWorldPoint(camera,{1.7f,0,0});Check(start&&end,"ray projected endpoints");
    Check(gizmo.Begin(rayEditor,GizmoMode::Translate,GizmoAxis::X,{start->x,start->y},{1,0},80,GizmoSpace::World,&camera)&&gizmo.Update(rayEditor,{end->x,end->y})&&gizmo.End(rayEditor),"perspective ray-axis drag");
    Check(Near(rayEditor.EditWorld().GetTransform(rayEditor.EditWorld().FindByStableId(id))->position,{1.7f,0,0},.005f),"ray drag world endpoint");
    Check(rayEditor.Undo(),"ray drag undo");
    Check(gizmo.Begin(rayEditor,GizmoMode::Translate,GizmoAxis::X,{start->x,start->y},{1,0},80,GizmoSpace::World,&camera),"projection capture");
    auto resized=camera;resized.viewport.width+=1;Check(!gizmo.ProjectionMatches(resized)&&gizmo.ProjectionMatches(camera)&&gizmo.End(rayEditor,false),"projection changes invalidate captured drag");
}

void Histories(const std::filesystem::path& directory)
{
    EditorSession editor;std::vector<std::string> scenes;std::vector<StableEntityId> selections;
    const auto capture=[&]{scenes.push_back(Logical(editor.EditWorld()));selections.push_back(editor.Selected());};capture();
    const auto root=editor.Create("root");capture();const auto child=editor.Create("child",root);capture();
    Check(editor.SetProperty(child,"transform.position",Vec3{1,2,3}),"history property");capture();
    Check(editor.SetMesh(child,{"shared mesh",{"shared material"}}),"history resource");capture();
    Check(editor.Rename(root,"renamed"),"history name");capture();
    const auto copy=editor.Duplicate(root);Check(copy,"history subtree copy");capture();
    Check(editor.SetParent(child,0,ReparentMode::KeepWorld),"history reparent");capture();
    Check(editor.Delete(root),"history subtree delete");capture();
    for(size_t index=scenes.size()-1;index>0;--index)Check(editor.Undo()&&Logical(editor.EditWorld())==scenes[index-1]&&editor.Selected()==selections[index-1],"mixed undo restores exact logical scene, resources, identities and selection");
    for(size_t index=1;index<scenes.size();++index)Check(editor.Redo()&&Logical(editor.EditWorld())==scenes[index]&&editor.Selected()==selections[index],"mixed redo restores exact logical scene");
    MaterialEditSession material;MaterialDocument initial;initial.name="材质历史";Check(material.Reset(initial,true)&&!material.Dirty(),"saved material session");
    Check(material.BeginEdit(),"material drag start");for(int index=1;index<=12;++index)Check(material.SetProperty("material.roughness",float(index)/20),"material drag update");
    Check(material.CommitEdit()&&material.UndoCount()==1&&material.Document().roughness==.6f,"material drag merges");
    Check(material.Undo()&&!material.Dirty()&&material.Document().roughness==1&&material.Redo()&&material.Dirty(),"material undo redo saved boundary");
    Check(material.BeginEdit()&&material.SetProperty("material.baseColor",Vec4{.2f,.3f,.4f,1})&&!material.MarkSaved()&&material.CancelEdit()&&material.Document().baseColor[0]==1,"material cancel and save guard");
    const std::string texture(32,'a');Check(material.SetProperty("material.normalTexture",texture)&&material.Undo()&&material.Document().normalTexture.empty()&&material.Redo()&&material.Document().normalTexture==texture,"texture reference history");
    const std::string shader(32,'b');Check(material.SetProperty("material.shader",shader)&&material.Undo()&&material.Document().shader.empty()&&material.Redo()&&material.Document().shader==shader,"shader reference history");
    const auto count=material.UndoCount();Check(!material.SetProperty("material.metallic",2.0f)&&material.UndoCount()==count,"bad property does not enter history");
    std::vector<AssetDiagnostic> diagnostics;const auto path=directory/"history.material.json";
    Check(SaveMaterialDocument(path,material.Document(),diagnostics)&&material.MarkSaved()&&!material.Dirty(),"save actual history document");
    MaterialDocument loaded;Check(LoadMaterialDocument(path,loaded,diagnostics)&&loaded.normalTexture==texture&&loaded.shader==shader&&loaded.roughness==.6f,"history persistence");
    Check(material.Undo()&&material.Dirty()&&material.Redo()&&!material.Dirty(),"undo after save restores dirty status");
    for(int index=0;index<140;++index)Check(material.SetProperty("material.metallic",float(index%100)/100),"bounded material history");
    Check(material.UndoCount()==100&&material.Undo()&&material.SetProperty("material.doubleSided",true)&&material.RedoCount()==0,"history limit and redo branch invalidation");
    auto invalid=loaded;invalid.roughness=-1;Check(!material.Reset(invalid,true)&&material.UndoCount()>0,"invalid reset preserves current document/history");
}
}
int main(int argc,char** argv)
{
    try
    {
        const auto directory=std::filesystem::path(argc>1?argv[1]:"out/validation/editor-transform")/asset_io::NewId();std::filesystem::create_directories(directory);
        Decomposition();Spaces();Histories(directory);
        std::cout<<"EditorTransformTests passed (TRS, mirrored scales, precise snapshots, local/world/ray gizmos, keep-world reparent, mixed and material history).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EditorTransformTests failed: "<<error.what()<<'\n';return 1;}
}
