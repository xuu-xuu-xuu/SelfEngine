#include "FunctionLayer/GameUI/UiControlGeometry.h"
#include "FunctionLayer/GameUI/UiEventSystem.h"
#include "FunctionLayer/GameUI/ControlsGallery.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
UiPoint Center(UiRect r){return {r.x+r.width/2,r.y+r.height/2};}
void Test()
{
    for(const float scale:{.5f,1.0f,1.5f})
    {
        UiTree tree;tree.SetCanvasReference(1280,720);const auto slider=tree.Create(UiKind::Slider),progress=tree.Create(UiKind::ProgressBar),checkbox=tree.Create(UiKind::Checkbox);
        UiStyle s;s.offset={20,20};s.size={300,36};tree.SetStyle(slider,s);s.offset.y=80;tree.SetStyle(progress,s);s.offset.y=140;tree.SetStyle(checkbox,s);tree.SetRange(slider,-10,90);tree.SetStep(slider,5);tree.SetValue(slider,40);tree.Layout(1280*scale,720*scale);
        const auto r=tree.Get(slider)->rectangle;auto g=MakeUiSliderGeometry(r,scale,40,-10,90);const UiPoint grab{g.thumb.x+g.thumb.width*.8f,g.thumb.y+g.thumb.height*.5f};tree.PointerDown(grab);Check(tree.Get(slider)->value==40,"grabbing slider thumb has no jump");tree.PointerMove({r.x+r.width+100,grab.y});tree.PointerUp({r.x+r.width+100,grab.y});Check(tree.Get(slider)->value==90,"slider max endpoint");
        tree.PointerDown({r.x+r.width-5*scale,grab.y});tree.PointerMove({r.x-100,grab.y});tree.PointerUp({r.x-100,grab.y});Check(tree.Get(slider)->value==-10,"slider min endpoint");tree.SetValue(slider,73);Check(tree.Get(slider)->value==75,"step relative to minimum");tree.Focus(slider);tree.EditKey(36,false,false);Check(tree.Get(slider)->value==-10,"keyboard minimum");tree.EditKey(35,false,false);Check(tree.Get(slider)->value==90,"keyboard maximum");
        tree.SetValue(progress,100);Check(tree.Get(progress)->value==1&&UiValueRatio(tree.Get(progress)->value,0,1)==1,"progress upper bound");tree.SetValue(progress,-100);Check(tree.Get(progress)->value==0&&UiValueRatio(tree.Get(progress)->value,0,1)==0,"progress lower bound");tree.SetRange(progress,42,42);Check(UiValueRatio(42,42,42)==0,"degenerate progress never NaN");
        tree.SetValue(checkbox,.3);Check(tree.Get(checkbox)->value==1,"checkbox bool canonical value");tree.PointerDown(Center(tree.Get(checkbox)->rectangle));tree.PointerUp(Center(tree.Get(checkbox)->rectangle));Check(tree.Get(checkbox)->value==0,"checkbox click toggles");
        const auto scroll=tree.Create(UiKind::ScrollView);s.offset={400,20};s.size={200,140};s.layout=UiLayout::Vertical;s.clip=true;tree.SetStyle(scroll,s);
        for(unsigned i=0;i<10;++i){const auto child=tree.Create(UiKind::Button,scroll);UiStyle item;item.size={200,30};tree.SetStyle(child,item);}tree.Layout(1280*scale,720*scale);
        auto n=tree.Get(scroll);auto bar=MakeUiScrollGeometry(n->rectangle,0,n->contentHeight*scale,n->scroll*scale,scale);Check(bar.scrollable,"scrollbar range");const UiPoint thumb{bar.thumb.x+bar.thumb.width/2,bar.thumb.y+bar.thumb.height*.8f};
        Check(UiEventSystem::HitTarget(tree,thumb)==scroll,"scrollbar topmost above overlapping child");tree.PointerDown(thumb);Check(tree.Get(scroll)->scroll==0,"thumb grab does not jump");tree.PointerMove({thumb.x,n->rectangle.y+n->rectangle.height+100});tree.PointerUp({thumb.x,n->rectangle.y+n->rectangle.height+100});Check(std::abs(tree.Get(scroll)->scroll-160)<.01,"scroll drag max logical offset");
        const auto visuals=tree.Visuals();for(const auto& v:visuals)if(v.parent==scroll){Check(v.depth==1,"snapshot hierarchy depth");const auto p=Center(v.rectangle);if(v.clip.Contains(p)&&v.rectangle.Contains(p)&&p.x<bar.track.x)Check(UiEventSystem::HitTarget(tree,p)==v.id,"scrolled child hit uses displayed coordinates");}
    }
}
}
int main(int argc,char** argv){try{Test();if(argc>1){const auto path=std::filesystem::path(argv[1]);std::filesystem::create_directories(path.parent_path());std::ofstream file(path);file<<"{\"success\":true,\"scales\":3,\"rangeStep\":true,\"thumbNoJump\":true,\"scrollOverlayHit\":true,\"progressBounds\":true,\"checkbox\":true}\n";}std::cout<<"UiControlsTests passed (shared geometry, range/step, thumb grab, scroll overlay/hit and scaling).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
