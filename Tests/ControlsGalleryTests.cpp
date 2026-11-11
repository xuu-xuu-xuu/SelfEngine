#include "FunctionLayer/GameUI/ControlsGallery.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)
static se::UiPoint Center(const se::UiNode* node){return {node->rectangle.x+node->rectangle.width*0.5f,node->rectangle.y+node->rectangle.height*0.5f};}
int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string error;
    CHECK(text.Load("Resources/Localization/zh-CN.txt",error));
    se::UiTree ui;se::ControlsGallery gallery;
    CHECK(se::BuildControlsGallery(ui,text,gallery));
    const auto sourceDocument=ui.SaveJson();
    CHECK(gallery.root==1&&gallery.title==2&&gallery.status==28&&ui.Count()==28);
    std::set<se::UiKind> kinds;for(auto id:ui.Ids())kinds.insert(ui.Get(id)->kind);CHECK(kinds.size()==9);
    const auto observe=[&]{const auto events=ui.TakeEvents();se::UpdateControlsGallery(ui,gallery,events,text);};
    const auto click=[&](se::UiId id){const auto p=Center(ui.Get(id));const bool down=ui.PointerDown(p),up=ui.PointerUp(p);observe();return down&&up;};
    CHECK(click(gallery.button)&&gallery.clicks==1);
    ui.Update(0.3);CHECK(ui.Get(gallery.animated)->style.offset.x>920&&ui.Get(gallery.animated)->style.offset.x<1060);
    CHECK(ui.Get(gallery.animated)->color.r>0.2f&&ui.Get(gallery.animated)->opacity<1);
    ui.Update(0.3);CHECK(std::abs(ui.Get(gallery.animated)->style.offset.x-1060)<0.01f);
    CHECK(click(gallery.checkbox)&&!ui.Get(gallery.image)->visible);
    CHECK(click(gallery.checkbox)&&ui.Get(gallery.image)->visible);
    ui.Layout(1280,720);
    auto slider=Center(ui.Get(gallery.slider));slider.x=ui.Get(gallery.slider)->rectangle.x+ui.Get(gallery.slider)->rectangle.width*0.73f;
    CHECK(ui.PointerDown(slider)&&ui.PointerUp(slider));observe();
    CHECK(ui.Get(gallery.slider)->value==75&&std::abs(ui.Get(gallery.progress)->value-0.75)<0.00001);
    CHECK(ui.Focus(gallery.textInput)&&ui.EditKey('A',false,true)&&ui.InsertText("中文😀"));observe();
    CHECK(ui.Get(gallery.status)->text.find("中文😀")!=std::string::npos);
    CHECK(ui.SetComposition("候选"));ui.CancelComposition();CHECK(ui.Get(gallery.textInput)->text=="中文😀");
    CHECK(click(gallery.scrollButtons.front()));
    CHECK(ui.Get(gallery.status)->text==text.GetText("UI.Gallery.ScrollClicked")+" 1");
    CHECK(ui.Wheel(Center(ui.Get(gallery.scroll)),-3)&&ui.Get(gallery.scroll)->scroll>0);
    ui.Layout(1280,720);
    const auto* scroll=ui.Get(gallery.scroll);const se::UiPoint track{scroll->rectangle.x+scroll->rectangle.width-3,scroll->rectangle.y+scroll->rectangle.height-10};
    CHECK(ui.PointerDown(track)&&ui.PointerUp(track)&&ui.Get(gallery.scroll)->scroll>100);
    ui.CancelInput();ui.Layout(640,360);CHECK(std::abs(ui.CanvasScale()-0.5f)<0.00001f);
    CHECK(click(gallery.button)&&gallery.clicks==2);
    ui.Update(0.6);ui.Layout(640,360);CHECK(std::abs(ui.Get(gallery.animated)->rectangle.x-460)<0.01f);
    const auto visuals=ui.Visuals();CHECK(visuals.size()==28);
    for(const auto& visual:visuals)if(visual.id==gallery.scrollButtons.back())CHECK(visual.clip.height<=225);
    se::UiTree roundtrip;CHECK(roundtrip.LoadJson(sourceDocument,error)&&roundtrip.Count()==28);
    if(argc==3&&std::wstring_view(argv[1])==L"--write")
    {
        const std::filesystem::path path=argv[2];std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<sourceDocument;CHECK(file.good());
    }
    std::cout<<"ControlsGalleryTests passed (9 kinds, capture, slider step, scroll track, Unicode, transitions, reference scaling).\n";
    return 0;
}
