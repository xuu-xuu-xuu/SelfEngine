#include "ToolLayer/Editor/EditorLayout.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string Read(const fs::path& path)
{
    std::ifstream input(path,std::ios::binary);Check(bool(input),"read fixture");
    return {std::istreambuf_iterator<char>(input),{}};
}
void Put(const fs::path& path,const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path,std::ios::binary|std::ios::trunc);output<<text;Check(bool(output),"write fixture");
}
bool Near(float a,float b){return std::abs(a-b)<.01f;}
void CheckWithin(const EditorPanelGeometry& value,float width,float height)
{
    Check(value.x>=0&&value.y>=0&&value.width>0&&value.height>0
        &&value.x+value.width<=width+.01f&&value.y+value.height<=height+.01f,"restored panel lies entirely inside current client area");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: EditorLayoutTests <output-dir>");
        const auto output=fs::absolute(fs::path(argv[1]))/asset_io::NewId();
        fs::create_directories(output);
        EditorLayout layout;
        Check(layout.Visible(EditorPanel::Hierarchy)&&layout.Visible(EditorPanel::Inspector)&&layout.Visible(EditorPanel::Assets)&&layout.Visible(EditorPanel::Viewport),"default panels visible");
        Check(!layout.Visible(EditorPanel::MaterialEditor)&&!layout.Visible(EditorPanel::UiDesigner),"default optional panels hidden");
        Check(!layout.Visible(EditorPanel::ProjectSettings)&&!layout.Visible(EditorPanel::BuildExport),"default project/build panels hidden");
        Check(layout.Visible(EditorPanel::Console),"console visible by default");
        Check(layout.VisibleCount()==5,"default visible count");
        Check(!layout.Geometry(EditorPanel::Viewport,1280,720),"default geometry is supplied by the frontend");
        Check(layout.CaptureGeometry(EditorPanel::Viewport,320,180,640,360,1280,720),"capture normalized viewport geometry");
        Check(!layout.CaptureGeometry(EditorPanel::Viewport,320,180,640,360,1280,720),"unchanged geometry creates no new revision");
        Check(!layout.CaptureGeometry(EditorPanel::Viewport,480,270,960,540,1920,1080),"same proportions at a different resolution are unchanged");
        for(const auto size:{std::pair{1280.0f,720.0f},std::pair{1920.0f,1080.0f}})
            for(const float scale:{1.0f,1.5f,2.0f})
            {
                const auto geometry=layout.Geometry(EditorPanel::Viewport,size.first,size.second,scale);
                Check(geometry.has_value(),"geometry is available at two sizes and three UI scales");
                Check(Near(geometry->x,size.first/4)&&Near(geometry->y,size.second/4)
                    &&Near(geometry->width,size.first/2)&&Near(geometry->height,size.second/2),"usable geometry retains its relative position and size");
                CheckWithin(*geometry,size.first,size.second);
            }
        const float nan=std::numeric_limits<float>::quiet_NaN(),infinity=std::numeric_limits<float>::infinity();
        Check(!layout.CaptureGeometry(EditorPanel::Viewport,nan,0,100,100,1280,720)
            &&!layout.CaptureGeometry(EditorPanel::Viewport,0,infinity,100,100,1280,720)
            &&!layout.CaptureGeometry(EditorPanel::Viewport,0,0,-100,100,1280,720)
            &&!layout.CaptureGeometry(EditorPanel::Viewport,0,0,100,0,1280,720)
            &&!layout.CaptureGeometry(EditorPanel::Viewport,0,0,100,100,0,720)
            &&!layout.CaptureGeometry(EditorPanel::Viewport,0,0,100,100,1280,nan),"invalid geometry and viewport dimensions cannot replace valid state");
        Check(Near(layout.Geometry(EditorPanel::Viewport,1280,720)->x,320),"invalid captures preserve the previous rectangle");
        Check(!layout.Geometry(EditorPanel::Viewport,0,720)&&!layout.Geometry(EditorPanel::Viewport,1280,infinity)
            &&!layout.Geometry(EditorPanel::Viewport,1280,720,0)&&!layout.Geometry(EditorPanel::Viewport,1280,720,nan),"invalid restore arguments produce no rectangle");
        Check(!layout.CaptureGeometry(static_cast<EditorPanel>(-1),0,0,100,100,1280,720)
            &&!layout.Geometry(static_cast<EditorPanel>(99),1280,720),"invalid panel identity is rejected");
        EditorLayout outside;
        Check(outside.CaptureGeometry(EditorPanel::Inspector,-256,1440,6400,3600,1280,720),"off-screen and oversize geometry is captured for constrained restore");
        for(const auto size:{std::pair{1280.0f,720.0f},std::pair{1920.0f,1080.0f},std::pair{64.0f,48.0f}})
            for(const float scale:{1.0f,1.5f,2.0f})
            {
                const auto geometry=outside.Geometry(EditorPanel::Inspector,size.first,size.second,scale);
                Check(geometry.has_value(),"off-screen layout restores in each supported scale");
                CheckWithin(*geometry,size.first,size.second);
                Check(Near(geometry->x,0)&&Near(geometry->width,size.first),"oversize layout clamps to the available width");
            }
        Check(outside.CaptureGeometry(EditorPanel::Inspector,5000,-100,1,1,1280,720),"capture an unusably small off-screen panel");
        for(const float scale:{1.0f,1.5f,2.0f})
        {
            const auto geometry=outside.Geometry(EditorPanel::Inspector,1280,720,scale);
            Check(geometry&&geometry->width>=160*scale&&geometry->height>=96*scale&&geometry->y>=24*scale,"restore guarantees scaled controls and leaves the menu accessible");
            CheckWithin(*geometry,1280,720);
        }
        layout.SetVisible(EditorPanel::UiDesigner,true);
        layout.SetVisible(EditorPanel::Assets,false);
        const auto path=output/"layout.json";
        std::string error;
        Check(layout.Save(path,error)&&fs::is_regular_file(path),"layout saved");
        EditorLayout loaded;
        Check(loaded.Load(path,error)&&loaded.Visible(EditorPanel::UiDesigner)&&!loaded.Visible(EditorPanel::Assets),"layout round trip");
        const auto restored=loaded.Geometry(EditorPanel::Viewport,1920,1080,2);
        Check(restored&&Near(restored->x,480)&&Near(restored->y,270)&&Near(restored->width,960)&&Near(restored->height,540),"geometry round trip retains proportions independently of DPI");
        Check(!layout.Visible(EditorPanel::Assets)&&layout.Visible(EditorPanel::UiDesigner),"save does not mutate current layout");
        EditorPanel parsed;
        Check(ParseEditorPanelId("viewport",parsed)&&parsed==EditorPanel::Viewport,"panel id parse");
        Check(ParseEditorPanelId("build-export",parsed)&&parsed==EditorPanel::BuildExport,"new panel id parse");
        Check(!ParseEditorPanelId("unknown-panel",parsed),"unknown panel id rejected");
        Put(output/"bad-schema.json","{\"schema\":2,\"panels\":[]}");
        const auto before=Read(output/"bad-schema.json");
        EditorLayout rejected;
        Check(!rejected.Load(output/"bad-schema.json",error)&&Read(output/"bad-schema.json")==before,"unsupported schema rejected without rewriting");
        Check(!rejected.Save(output/"bad-schema.json",error)&&Read(output/"bad-schema.json")==before,"automatic save cannot replace a future schema");
        rejected.ResetDefaults();
        Check(!rejected.Save(output/"bad-schema.json",error)&&Read(output/"bad-schema.json")==before,"reset does not downgrade a future schema");
        Put(output/"unknown.json","{\"schema\":1,\"panels\":[{\"id\":\"future-panel\",\"visible\":true},{\"id\":\"hierarchy\",\"visible\":false}]}");
        EditorLayout filtered;
        Check(filtered.Load(output/"unknown.json",error)&&!filtered.Visible(EditorPanel::Hierarchy),"unknown panel ignored, known panel applied");
        Check(!filtered.Geometry(EditorPanel::Viewport,1280,720),"legacy visibility-only layout has no invented geometry");
        Put(output/"all-hidden.json","{\"schema\":1,\"panels\":[{\"id\":\"hierarchy\",\"visible\":false},{\"id\":\"inspector\",\"visible\":false},{\"id\":\"assets\",\"visible\":false},{\"id\":\"material\",\"visible\":false},{\"id\":\"viewport\",\"visible\":false},{\"id\":\"ui-designer\",\"visible\":false},{\"id\":\"project-settings\",\"visible\":false},{\"id\":\"build-export\",\"visible\":false},{\"id\":\"console\",\"visible\":false}]}");
        EditorLayout hidden;
        Check(!hidden.Load(output/"all-hidden.json",error)&&hidden.Visible(EditorPanel::Hierarchy),"all-hidden layout rejected");
        Put(output/"broken.json","{ broken");
        EditorLayout broken;
        Check(!broken.Load(output/"broken.json",error)&&!broken.LastError().empty(),"broken layout rejected with reason");
        Check(!broken.Save(output/"broken.json",error)&&Read(output/"broken.json")=="{ broken","automatic save preserves corrupt bytes without a backup");
        broken.ResetDefaults();
        Check(broken.Save(output/"broken.json",error),"explicit reset can replace the corrupt bytes previously shown to the user");
        const auto first=Read(path);
        layout.SetVisible(EditorPanel::Assets,true);
        Check(layout.CaptureGeometry(EditorPanel::Viewport,128,72,640,360,1280,720)&&layout.Save(path,error),"save second valid layout revision");
        auto backup=path;backup+=".bak";
        Check(Read(backup)==first,"backup contains the exact previous valid revision");
        const auto second=Read(path);
        Put(path,"{ interrupted write");
        EditorLayout recovered;
        Check(recovered.Load(path,error)&&error=="Editor.LayoutRecoveredBackup"&&!recovered.LastError().empty(),"corrupt main uses a valid backup and reports recovery");
        Check(!recovered.Visible(EditorPanel::Assets)&&recovered.Visible(EditorPanel::UiDesigner),"backup restores the previous visibility state");
        Check(Near(recovered.Geometry(EditorPanel::Viewport,1280,720)->x,320),"backup restores the previous geometry");
        recovered.SetVisible(EditorPanel::Assets,true);
        Check(!recovered.Save(path,error)&&Read(path)=="{ interrupted write"&&Read(backup)==first,"editing recovered layout cannot erase the bad main or last good backup");
        recovered.ResetDefaults();
        Check(recovered.Save(path,error)&&Read(backup)==first,"explicit reset never promotes corrupt main bytes into the backup");
        EditorLayout defaults;
        Check(defaults.Load(path,error)&&defaults.Visible(EditorPanel::Assets)&&!defaults.Geometry(EditorPanel::Viewport,1280,720),"reset revision restores default visibility and clears geometry");
        Put(path,"{ broken again");Put(backup,"{ also broken");
        Check(!recovered.Load(path,error)&&recovered.Visible(EditorPanel::Assets)&&!recovered.Geometry(EditorPanel::Viewport,1280,720),"two corrupt files fall back to defaults rather than old in-memory state");
        Check(!recovered.Save(path,error)&&Read(path)=="{ broken again"&&Read(backup)=="{ also broken","invalid fallback candidates stay untouched");
        recovered.ResetDefaults();Put(path,"{ changed after reset");
        Check(!recovered.Save(path,error)&&Read(path)=="{ changed after reset","reset approval cannot overwrite a different external revision");
        Put(output/"bad-schema.json.bak",first);
        Check(!rejected.Load(output/"bad-schema.json",error)&&!rejected.Visible(EditorPanel::UiDesigner),"future schema does not silently load an older backup");
        Check(Read(output/"bad-schema.json")==before&&Read(output/"bad-schema.json.bak")==first,"future schema and backup both remain intact");
        EditorLayout unobserved;
        Check(!unobserved.Save(path,error)&&Read(path)=="{ changed after reset","save also validates a destination never loaded by this object");
        const auto blockedBackup=output/"backup-blocked.json";
        Put(blockedBackup,second);fs::create_directories(output/"backup-blocked.json.bak");
        Check(!layout.Save(blockedBackup,error)&&error=="Editor.LayoutBackupWriteFailed"&&Read(blockedBackup)==second,"backup failure preserves the valid main file");
        Put(output/"bad-geometry.json","{\"schema\":1,\"panels\":[{\"id\":\"viewport\",\"visible\":true,\"geometry\":{\"x\":0,\"y\":0,\"width\":0,\"height\":1}}]}");
        Check(!unobserved.Load(output/"bad-geometry.json",error),"zero geometry in persisted data is rejected");
        Put(output/"bad-number.json","{\"schema\":1,\"panels\":[{\"id\":\"viewport\",\"visible\":true,\"geometry\":{\"x\":1e100,\"y\":0,\"width\":1,\"height\":1}}]}");
        Check(!unobserved.Load(output/"bad-number.json",error),"geometry beyond finite float range is rejected");
        Check(loaded.Load(output/"absent.json",error)&&loaded.Visible(EditorPanel::Assets)&&!loaded.Geometry(EditorPanel::Viewport,1280,720),"missing project layout starts from defaults without leaking previous project geometry");
        for(const auto& entry:fs::recursive_directory_iterator(output))
            Check(entry.path().filename().string().find(".tmp-")==std::string::npos,"failed writes leave no temporary candidate files");
        Check(!EditorLayout::DefaultPath().empty(),"default layout path resolved");
        std::cout<<"EditorLayoutTests passed (proportional geometry, two sizes/three scales, visible bounds, legacy schema, backup recovery, failed-write and corrupt-file protection).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"EditorLayoutTests failed: "<<failure.what()<<'\n';return 1;
    }
}
