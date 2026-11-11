#include "FunctionLayer/GameUI/UiTree.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Rect(UiRect a,UiRect b){const bool same=std::abs(a.x-b.x)<.01f&&std::abs(a.y-b.y)<.01f&&std::abs(a.width-b.width)<.01f&&std::abs(a.height-b.height)<.01f;if(!same)std::cerr<<"actual="<<a.x<<','<<a.y<<','<<a.width<<','<<a.height<<" expected="<<b.x<<','<<b.y<<','<<b.width<<','<<b.height<<'\n';Check(same,"layout rectangle agrees with independent expected value");}
void Compare(const UiTree& a,const UiTree& b)
{
    Check(a.Ids()==b.Ids(),"same IDs after complete rebuild");
    for(const auto id:a.Ids())
    {
        const auto& x=*a.Get(id);const auto& y=*b.Get(id);Check(x.style==y.style&&x.visible==y.visible&&x.children==y.children,"persistent layout properties and order agree");
        bool visible=true;for(auto* node=&x;node;node=a.Get(node->parent))visible=visible&&node->visible;
        if(!visible)continue; // 隐藏分支不参与布局，可保留旧派生矩形；显示后再验证。
        try{Rect(x.rectangle,y.rectangle);Rect(x.clipping,y.clipping);}catch(...){std::cerr<<"node="<<id<<" visible="<<x.visible<<'\n';throw;}
        Check(std::abs(x.contentHeight-y.contentHeight)<.01f&&std::abs(x.scroll-y.scroll)<.01f,"content extent and scroll agree with full rebuild");
    }
}
void Rebuilt(const UiTree& tree,float width,float height)
{UiTree reference;std::string error;Check(reference.LoadJson(tree.SaveJson(),error),"load reference tree");reference.Layout(width,height);Compare(tree,reference);}
void Ids()
{
    UiTree tree;const auto root=tree.Create(UiKind::Panel),removed=tree.Create(UiKind::Panel,root);Check(tree.Remove(removed),"remove highest UI ID");
    auto saved=tree.SaveJson();UiTree loaded;std::string error;Check(loaded.LoadJson(saved,error)&&loaded.Create(UiKind::Panel)>removed,"saved next ID prevents reuse after highest node deletion");
    auto document=nlohmann::json::parse(saved);document.erase("nextId");Check(loaded.LoadJson(document.dump(),error),"legacy document without next ID loads");
    const auto before=loaded.SaveJson();for(const auto invalid:{nlohmann::json(0),nlohmann::json(-1),nlohmann::json(1.5)})
    {document["nextId"]=invalid;Check(!loaded.LoadJson(document.dump(),error)&&loaded.SaveJson()==before,"invalid next ID load preserves complete tree");}
}
void Anchors()
{
    UiTree tree;Check(tree.SetCanvasReference(800,600),"reference canvas");const auto root=tree.Create(UiKind::Panel);UiStyle fill;fill.anchorMax={1,1};fill.offset={12,18};fill.size={-42,-56};fill.clip=true;tree.SetStyle(root,fill);
    const auto center=tree.Create(UiKind::Panel,root);UiStyle middle;middle.anchorMin=middle.anchorMax=middle.pivot={.5f,.5f};middle.size={100,40};tree.SetStyle(center,middle);
    const auto bottom=tree.Create(UiKind::Panel,root);UiStyle edge;edge.anchorMin=edge.anchorMax=edge.pivot={1,1};edge.offset={-7,-9};edge.size={80,30};tree.SetStyle(bottom,edge);
    for(const UiPoint size:{UiPoint{640,360},UiPoint{800,600},UiPoint{1280,720},UiPoint{1920,1080},UiPoint{2560,1440},UiPoint{1600,1200},UiPoint{2400,1800}})
    {
        tree.Layout(size.x,size.y);const float scale=std::min(size.x/800,size.y/600),width=size.x-42*scale,height=size.y-56*scale;
        Rect(tree.Get(root)->rectangle,{12*scale,18*scale,width,height});
        Rect(tree.Get(center)->rectangle,{12*scale+width/2-50*scale,18*scale+height/2-20*scale,100*scale,40*scale});
        Rect(tree.Get(bottom)->rectangle,{12*scale+width-87*scale,18*scale+height-39*scale,80*scale,30*scale});
        Rect(tree.Get(center)->clipping,tree.Get(root)->rectangle);Rebuilt(tree,size.x,size.y);
        tree.Layout(size.x,size.y);Check(tree.LayoutStatistics().arrangedNodes==0,"stable canvas performs no arrangement");
    }
}
void DirtyBranches()
{
    UiTree tree;const auto root=tree.Create(UiKind::Panel);UiStyle canvas;canvas.size={800,600};tree.SetStyle(root,canvas);
    const auto branch=tree.Create(UiKind::Panel,root),leaf=tree.Create(UiKind::Label,branch),sibling=tree.Create(UiKind::Panel,root);tree.Layout(1600,1200);
    auto style=tree.Get(leaf)->style;style.offset={8,9};tree.SetStyle(leaf,style);tree.Layout(1600,1200);
    Check(tree.LayoutStatistics().arrangedNodes==1&&!tree.LayoutStatistics().fullPass,"absolute leaf only arranges itself");Rebuilt(tree,1600,1200);
    style=tree.Get(branch)->style;style.offset={20,30};tree.SetStyle(branch,style);tree.Layout(1600,1200);
    Check(tree.LayoutStatistics().arrangedNodes==2,"moving an absolute branch only updates its descendants");Rebuilt(tree,1600,1200);
    tree.SetText(leaf,"layout unchanged");tree.SetColor(leaf,{.5f,.5f,.5f,1});tree.SetStyle(branch,style);tree.Layout(1600,1200);Check(tree.LayoutStatistics().arrangedNodes==0,"text/color and equal style do not invalidate explicit layout");
    UiStyle horizontal;horizontal.layout=UiLayout::Horizontal;horizontal.size={400,100};horizontal.padding=10;horizontal.spacing=4;tree.SetStyle(sibling,horizontal);
    const auto a=tree.Create(UiKind::Button,sibling),b=tree.Create(UiKind::Button,sibling);tree.Layout(1600,1200);style=tree.Get(a)->style;style.size.x=200;tree.SetStyle(a,style);tree.Layout(1600,1200);
    Check(tree.LayoutStatistics().arrangedNodes==3,"flow mutation arranges dependent parent and siblings only");Check(std::abs(tree.Get(b)->rectangle.x-(tree.Get(a)->rectangle.x+204))<.01f,"horizontal sibling follows changed width");Rebuilt(tree,1600,1200);
    tree.SetVisible(a,false);tree.Layout(1600,1200);Rebuilt(tree,1600,1200);
    Check(tree.Reparent(leaf,b),"reparent to another branch");tree.Layout(1600,1200);Rebuilt(tree,1600,1200);
    Check(tree.Remove(a),"remove hidden child");tree.Layout(1600,1200);Rebuilt(tree,1600,1200);
    tree.SetCanvasReference(800,600);tree.Layout(1600,1200);Rebuilt(tree,1600,1200);style=tree.Get(leaf)->style;style.offset.x+=1;tree.SetStyle(leaf,style);tree.Layout(1600,1200);Rebuilt(tree,1600,1200);
    tree.SetVisible(branch,false);style=tree.Get(branch)->style;style.size.x=300;tree.SetStyle(branch,style);tree.Layout(1920,1080);Rebuilt(tree,1920,1080);tree.SetVisible(branch,true);tree.Layout(1920,1080);Rebuilt(tree,1920,1080);
    tree.MoveTo(leaf,{6,7},.5);tree.Update(.25);tree.Layout(1920,1080);Rebuilt(tree,1920,1080);
}
void ScrollClamp()
{
    UiTree tree;tree.SetCanvasReference(800,600);const auto scroll=tree.Create(UiKind::ScrollView);UiStyle s;s.size={120,100};s.layout=UiLayout::Vertical;s.padding=4;tree.SetStyle(scroll,s);
    for(unsigned i=0;i<8;++i)tree.Create(UiKind::Button,scroll);tree.Layout(1600,1200);Check(tree.Wheel({30,30},-100)&&tree.Get(scroll)->scroll>0,"scaled scroll moves within logical range");
    s.size.y=400;tree.SetStyle(scroll,s);tree.Layout(1600,1200);Check(tree.Get(scroll)->scroll==0&&tree.LayoutStatistics().scrollClamps==1&&tree.LayoutStatistics().arrangedNodes<=tree.Count()*2,"clamp uses at most two subtree passes");Rebuilt(tree,1600,1200);
    auto document=nlohmann::json::parse(tree.SaveJson());for(auto& node:document["nodes"])if(node["kind"]==static_cast<int>(UiKind::ScrollView))node["scroll"]=10000;
    std::string error;UiTree clamped;Check(clamped.LoadJson(document.dump(),error),"load over-scrolled document");clamped.Layout(2400,1800);Check(clamped.LayoutStatistics().arrangedNodes<=clamped.Count()*2,"loaded clamp bounded");Rebuilt(clamped,2400,1800);
    UiTree nested;UiId parent=0;
    for(unsigned level=0;level<8;++level){const auto id=nested.Create(UiKind::ScrollView,parent);UiStyle style;style.size={120,level?1200.0f:100.0f};style.layout=UiLayout::Vertical;nested.SetStyle(id,style);parent=id;}
    const auto tail=nested.Create(UiKind::Panel,parent);UiStyle longChild;longChild.size={100,5000};nested.SetStyle(tail,longChild);document=nlohmann::json::parse(nested.SaveJson());for(auto& node:document["nodes"])if(node["kind"]==static_cast<int>(UiKind::ScrollView))node["scroll"]=10000;
    Check(nested.LoadJson(document.dump(),error),"load nested scroll scopes");nested.Layout(800,600);Check(nested.LayoutStatistics().scrollClamps==8&&nested.LayoutStatistics().arrangedNodes<=nested.Count()*2,"nested clamps merge into a bounded second pass");Rebuilt(nested,800,600);
}
void Random()
{
    std::mt19937 random(5612026);UiTree tree;tree.SetCanvasReference(800,600);const auto root=tree.Create(UiKind::Panel);UiStyle fill;fill.anchorMax={1,1};fill.size={0,0};tree.SetStyle(root,fill);std::vector<UiId> ids{root};
    for(unsigned i=0;i<127;++i)
    {
        const auto parent=ids[random()%ids.size()];const auto id=tree.Create(UiKind::Panel,parent);Check(id!=0,"random bounded tree");ids.push_back(id);UiStyle s;
        s.anchorMin=s.anchorMax={float(random()%3)*.5f,float(random()%3)*.5f};s.pivot={float(random()%3)*.5f,float(random()%3)*.5f};s.offset={float(random()%20),float(random()%20)};s.size={float(20+random()%80),float(20+random()%60)};
        s.layout=static_cast<UiLayout>(random()%3);s.clip=random()%2!=0;s.padding=float(random()%4);s.spacing=float(random()%4);tree.SetStyle(id,s);
    }
    for(unsigned step=0;step<256;++step)
    {
        const auto id=ids[1+random()%(ids.size()-1)];auto s=tree.Get(id)->style;s.offset.x+=float(int(random()%7)-3);s.size.y+=float(random()%3);tree.SetStyle(id,s);
        const float width=step%8==0?1920.0f:1280.0f,height=step%8==0?1080.0f:720.0f;tree.Layout(width,height);Check(tree.LayoutStatistics().arrangedNodes<=tree.Count()*2,"random arrangement budget");Rebuilt(tree,width,height);
    }
}
void WideTree()
{
    UiTree tree;const auto root=tree.Create(UiKind::Panel);std::vector<UiId> ids;
    for(unsigned i=0;i<4096;++i)ids.push_back(tree.Create(UiKind::Label,root));tree.Layout(1280,720);
    for(const auto id:ids){auto style=tree.Get(id)->style;style.offset={float(id%100),float(id/100)};tree.SetStyle(id,style);}
    tree.Layout(1280,720);Check(tree.LayoutStatistics().arrangedNodes==4096,"wide absolute siblings merge without arranging their parent");Rebuilt(tree,1280,720);
}
}
int main(int argc,char** argv)
{try{Ids();Anchors();DirtyBranches();ScrollClamp();Random();WideTree();const auto root=std::filesystem::path(argc>1?argv[1]:"out/validation/ui-layout");std::filesystem::create_directories(root);std::ofstream proof(root/"verification.json");proof<<"{\"schema\":1,\"success\":true,\"window_sizes\":7,\"random_seed\":5612026,\"random_nodes\":128,\"random_changes\":256,\"wide_nodes\":4096,\"leaf_arranged\":1,\"branch_arranged\":2,\"flow_arranged\":3,\"unchanged_arranged\":0,\"maximum_arrange_passes\":2,\"next_id_preserved\":true}\n";Check(bool(proof),"write layout proof");std::cout<<"UILayoutTests passed (anchors, scale, branches, flow, clamping, IDs and full rebuild reference).\n";return 0;}catch(const std::exception& error){std::cerr<<"UILayoutTests failed: "<<error.what()<<'\n';return 1;}}
