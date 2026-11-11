#include "FunctionLayer/Rendering/GameUIRenderer.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
UiVisual Visual(UiKind kind,UiRect rectangle,std::string text={})
{
    UiVisual result{};result.id=1;result.kind=kind;result.rectangle=rectangle;result.clip={0,0,640,480};result.text=std::move(text);result.opacity=1;result.enabled=true;result.color={1,1,1,1};result.fontSize=20;return result;
}
void Test()
{
    GameUIRenderer renderer;std::vector<AssetDiagnostic> diagnostics;
    std::vector<UiVisual> visuals{Visual(UiKind::Panel,{10,10,100,100}),Visual(UiKind::Panel,{20,20,100,100})};
    visuals.back().color={1,0,0,.5f};
    Check(renderer.Prepare(visuals,640,480,{1,1},nullptr,diagnostics),"prepare panels");
    Check(renderer.Geometry().commands.size()==1&&renderer.Geometry().commands[0].indexCount==12,"compatible adjacent panels merge");
    Check(renderer.Geometry().vertices[0].color==0xffffffff&&renderer.Geometry().vertices[4].color==0x800000ff,"transparent vertex order and linear alpha");
    visuals.push_back(Visual(UiKind::Image,{30,30,30,30}));visuals.push_back(visuals[0]);
    Check(renderer.Prepare(visuals,640,480,{1,1},nullptr,diagnostics)&&renderer.Geometry().commands.size()==3,"texture changes preserve painter order");
    Check(renderer.Geometry().commands[0].texture==renderer.Geometry().commands[2].texture&&renderer.Geometry().commands[0].texture!=renderer.Geometry().commands[1].texture,"no texture sorting across image");
    UiTree tree;const auto parent=tree.Create(UiKind::Panel);UiStyle style;style.offset={10,20};style.size={100,90};style.clip=true;Check(tree.SetStyle(parent,style),"clip parent");
    const auto child=tree.Create(UiKind::Image,parent);style.offset={70,60};style.size={80,70};style.clip=false;Check(tree.SetStyle(child,style),"clip child");tree.Layout(640,480);
    Check(renderer.Prepare(tree.Visuals(),640,480,{1,1},nullptr,diagnostics),"flattened clip stack");
    const auto clip=renderer.Geometry().commands.back().clip;Check(clip.x==10&&clip.y==20&&clip.z==110&&clip.w==110,"nested clipping uses parent intersection");
    auto label=Visual(UiKind::Label,{10,10,600,60},"中文 English \xf4\x8f\xbf\xbf");
    Check(renderer.Prepare({&label,1},640,480,{1,1},nullptr,diagnostics)&&!renderer.Geometry().vertices.empty(),"CJK English and missing glyph fallback");
    const auto old=renderer.Geometry();const auto warm=renderer.Statistics();
    for(unsigned i=0;i<20;++i)Check(renderer.Prepare({&label,1},640,480,{1,1},nullptr,diagnostics),"repeat text");
    Check(renderer.Statistics().layoutMisses==warm.layoutMisses&&renderer.Statistics().fontPages==warm.fontPages,"repeat text creates no new CPU cache entries or pages");
    std::string growth;for(unsigned i=0;i<40;++i)growth+="W"+std::to_string(i)+" ";
    auto large=Visual(UiKind::Label,{0,0,4000,4000},growth);large.fontSize=180;
    Check(renderer.Prepare({&large,1},4096,4096,{1,1},nullptr,diagnostics)&&renderer.Statistics().fontPages>warm.fontPages,"grow fixed size atlas pages");
    Check(renderer.Prepare({&label,1},640,480,{1,1},nullptr,diagnostics),"old text after growth");
    Check(renderer.Geometry().vertices.size()==old.vertices.size(),"old run quad count");
    for(size_t i=0;i<old.vertices.size();++i)Check(renderer.Geometry().vertices[i].uv.x==old.vertices[i].uv.x&&renderer.Geometry().vertices[i].uv.y==old.vertices[i].uv.y,"atlas growth never moves old glyph UV");
    for(unsigned i=0;i<280;++i){auto item=Visual(UiKind::Label,{0,0,600,60},"layout "+std::to_string(i));Check(renderer.Prepare({&item,1},640,480,{1,1},nullptr,diagnostics),"bounded layout variants");}
    Check(renderer.Statistics().layoutEntries==256,"layout LRU budget");
    Check(renderer.Prepare({&label,1},1280,960,{2,2},nullptr,diagnostics)&&renderer.Geometry().displaySize.x==640,"DPI keeps logical coordinates");
    auto invalid=visuals;invalid[0].rectangle.x=std::numeric_limits<float>::quiet_NaN();
    Check(!renderer.Prepare(invalid,640,480,{1,1},nullptr,diagnostics)&&renderer.Geometry().vertices.empty(),"invalid frame publishes no partial geometry");
    auto panel=Visual(UiKind::Panel,{0,0,10,10});std::vector<UiVisual> overBudget(65537,panel);
    Check(!renderer.Prepare(overBudget,640,480,{1,1},nullptr,diagnostics)&&renderer.Geometry().commands.empty(),"quad budget bounded atomically");
    renderer.Clear();Check(renderer.Statistics().fontPages==0&&renderer.Statistics().layoutEntries==0&&renderer.Geometry().vertices.empty(),"clear releases font and layout owners");
}
}
int main(){try{Test();std::cout<<"GameUIRenderTests passed (painter order, adjacent batches, nested clipping, glyph fallback, atlas growth, bounded caches and frame budget).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
