#include "ToolLayer/Editor/UICanvasEditor.h"
#include "ToolLayer/Editor/ImGuiEditor.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool Near(float a,float b){return std::abs(a-b)<.001f;}
void Geometry()
{
    UiTree ui;Check(ui.SetCanvasReference(320,180),"reference");const auto root=ui.Create(UiKind::Panel),child=ui.Create(UiKind::Button,root);UiStyle parent;parent.size={320,180};parent.padding=10;Check(ui.SetStyle(root,parent),"parent style");
    UiStyle style;style.anchorMin={.25f,.25f};style.anchorMax={.75f,.75f};style.pivot={.5f,.5f};style.offset={5,7};style.size={80,40};Check(ui.SetStyle(child,style),"anchored child");
    UICanvasEditor canvas;Check(canvas.SetPreview({640,360})&&canvas.Prepare(ui,{100,50,960,600}),"preview mapping");Check(Near(canvas.ViewScale(),1.5f)&&Near(canvas.Content().y,80)&&ui.CanvasScale()==2,"separate preview and design scales");const auto saved=ui.SaveJson();const auto anchors=canvas.Anchors(ui,child);Check(anchors&&Near(anchors->minimum.x,355)&&Near(anchors->minimum.y,230)&&Near(anchors->maximum.x,805)&&Near(anchors->maximum.y,470),"anchors follow padded parent");
    const auto r=ui.Get(child)->rectangle;const auto point=canvas.ToScreen(UiPoint{r.x+30,r.y+30});Check(canvas.Hit(ui,point)==child&&Near(canvas.ToPreview(point).x,r.x+30),"screen hit and inverse mapping");
    Check(canvas.Begin(ui,child,UiCanvasDrag::Move,point)&&canvas.Update(ui,{point.x+30,point.y+15}),"move in screen coordinates");Check(Near(ui.Get(child)->style.offset.x,15)&&Near(ui.Get(child)->style.offset.y,12),"move uses both scale factors");Check(canvas.End(ui,false)&&ui.SaveJson()==saved,"cancel restores design exactly");
    Check(canvas.Begin(ui,child,UiCanvasDrag::Resize,point)&&canvas.Update(ui,{point.x+60,point.y+30}),"resize anchored pivot");const auto resized=ui.Get(child);Check(Near(resized->rectangle.x,r.x)&&Near(resized->rectangle.y,r.y)&&Near(resized->rectangle.width,r.width+40)&&Near(resized->rectangle.height,r.height+20),"resize keeps left/top and changes design dimensions");canvas.End(ui,false);
    Check(canvas.Begin(ui,child,UiCanvasDrag::Resize,point)&&canvas.Update(ui,{point.x-10000,point.y-10000}),"resize minimum");Check(Near(ui.Get(child)->rectangle.width,2)&&Near(ui.Get(child)->rectangle.height,2),"one design unit minimum with stretched anchors");canvas.End(ui,false);
    const std::array<UiPoint,3> sizes{{{640,360},{1280,720},{1920,1080}}};
    for(const auto size:sizes){Check(canvas.SetPreview(size)&&canvas.Prepare(ui,{30,40,800,600}),"window preview");Check(ui.SaveJson()==saved,"preview does not modify design data");const auto visuals=canvas.Visuals(ui);Check(visuals.size()==2&&Near(visuals[0].rectangle.x,canvas.Content().x-30)&&Near(visuals[1].fontSize,18*ui.CanvasScale()*canvas.ViewScale()),"runtime visual transform preserves font/layout scale");}
    Check(canvas.Begin(ui,child,UiCanvasDrag::Move,canvas.ToScreen(UiPoint{ui.Get(child)->rectangle.x+5,ui.Get(child)->rectangle.y+5})),"begin before projection change");Check(canvas.SetPreview({640,360})&&canvas.Prepare(ui,{40,40,800,600})&&!canvas.Update(ui,{50,50}),"projection change rejects drag");canvas.End(ui,false);Check(ui.SaveJson()==saved&&!canvas.SetPreview({0,0})&&canvas.PreviewSize()==UiPoint{640,360},"bad preview preserves state");
    UiTree flow;const auto p=flow.Create(UiKind::Panel),c=flow.Create(UiKind::Button,p);parent.size={300,200};parent.padding=0;parent.layout=UiLayout::Vertical;flow.SetStyle(p,parent);canvas.Prepare(flow,{0,0,640,360});Check(!canvas.Begin(flow,c,UiCanvasDrag::Move,{10,10})&&canvas.LastError()=="EditorUi.CanvasFlowPosition","flow position is explicitly controlled");Check(canvas.Begin(flow,c,UiCanvasDrag::Resize,{10,10})&&canvas.Update(flow,{30,20})&&canvas.End(flow),"flow child can resize");
    flow.SetEnabled(c,false);Check(canvas.Hit(flow,{10,10})==c,"disabled control remains editable");flow.SetVisible(c,false);canvas.Prepare(flow,{0,0,640,360});Check(canvas.Hit(flow,{10,10})==p,"hidden child is not selected");
}
void BindingsAndHistory()
{
    UiTree ui;const auto root=ui.Create(UiKind::Panel),child=ui.Create(UiKind::Button,root);const auto legacy=ui.SaveJson();std::vector<UiEventBinding> bindings{{UiEventKind::Click,99,"sample.menu_flow","menu.start"}};Check(ui.SetBindings(child,bindings)&&ui.SetBindings(child,ui.Get(child)->bindings),"binding and self-span copy");const auto bound=ui.SaveJson();UiTree loaded;std::string error;Check(loaded.LoadJson(bound,error)&&loaded.Get(child)->bindings==bindings&&loaded.SaveJson()==bound,"bindings roundtrip");
    bindings.push_back(bindings.front());Check(!ui.SetBindings(child,bindings)&&ui.SaveJson()==bound,"duplicate event binding rejected atomically");bindings.resize(1);bindings[0].action="invalid action";Check(!ui.SetBindings(child,bindings)&&ui.SaveJson()==bound,"identifier validation");
    auto bad=nlohmann::json::parse(bound);bad["nodes"][1]["bindings"][0]["entity"]=-1;Check(!loaded.LoadJson(bad.dump(),error)&&error=="UI.InvalidBinding"&&loaded.SaveJson()==bound,"bad binding load preserves full document");Check(loaded.LoadJson(legacy,error)&&loaded.Get(child)->bindings.empty(),"old document has no bindings");
    ImGuiEditor editor;std::vector<std::string> history{ui.SaveJson()};const auto edit=[&](const auto& action){Check(editor.EditUi(ui,action),"UI history edit");history.push_back(ui.SaveJson());};
    edit([&](UiTree& tree){return tree.SetText(child,"菜单 Button");});edit([&](UiTree& tree){auto s=tree.Get(child)->style;s.offset={40,20};s.size={200,60};return tree.SetStyle(child,s);});
    edit([&](UiTree& tree){const std::array<UiEventBinding,1> value{{{UiEventKind::Click,100,"sample.menu_flow","menu.resume"}}};return tree.SetBindings(child,value);});
    edit([&](UiTree& tree){return tree.Reparent(child,0);});edit([&](UiTree& tree){return tree.Remove(root);});edit([&](UiTree& tree){return tree.Remove(child);});
    for(size_t i=history.size()-1;i>0;--i)Check(editor.UiUndo(ui)&&ui.SaveJson()==history[i-1],"mixed undo restores exact document");for(size_t i=1;i<history.size();++i)Check(editor.UiRedo(ui)&&ui.SaveJson()==history[i],"mixed redo restores exact document");
}
}
int main(int argc,char** argv)
{
    try{Geometry();BindingsAndHistory();const std::filesystem::path path=argc>1?argv[1]:"out/validation/ui-canvas/verification.json";std::filesystem::create_directories(path.parent_path());std::ofstream report(path);report<<nlohmann::json{{"success",true},{"previewSizes",3},{"coordinates",true},{"anchors",true},{"resizePivot",true},{"minimumSize",true},{"cancel",true},{"flow",true},{"designUnchanged",true},{"bindings",true},{"legacy",true},{"atomicLoad",true},{"mixedHistory",6}}.dump(2);Check(bool(report),"canvas evidence");std::cout<<"UICanvasEditorTests passed (coordinates, anchors, move/resize, preview isolation, binding schema and history).\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
