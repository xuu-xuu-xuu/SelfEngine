#include "EditorLayout.h"
#include "EditorSessionStore.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <utility>

namespace se
{
namespace
{
using Json=nlohmann::json;
constexpr std::size_t EditorLayoutSchema=1;
constexpr std::uintmax_t MaximumLayoutBytes=1024*1024;
std::size_t Index(EditorPanel panel){return static_cast<std::size_t>(panel);}
bool ValidPanel(EditorPanel panel){return Index(panel)<EditorPanelCount;}
std::array<EditorPanelState,EditorPanelCount> DefaultPanels()
{
    std::array<EditorPanelState,EditorPanelCount> panels{};
    for(const auto panel:{EditorPanel::UiDesigner,EditorPanel::MaterialEditor,EditorPanel::ProjectSettings,EditorPanel::BuildExport})
        panels[Index(panel)].visible=false;
    return panels;
}
bool ValidGeometry(const EditorPanelGeometry& value)
{
    return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.width)&&std::isfinite(value.height)
        &&value.width>0&&value.height>0;
}
bool SameGeometry(const EditorPanelGeometry& a,const EditorPanelGeometry& b)
{
    return std::abs(a.x-b.x)<=1e-6f&&std::abs(a.y-b.y)<=1e-6f
        &&std::abs(a.width-b.width)<=1e-6f&&std::abs(a.height-b.height)<=1e-6f;
}
std::filesystem::path PathIdentity(const std::filesystem::path& path)
{
    std::error_code error;
    const auto canonical=std::filesystem::weakly_canonical(path,error);
    return error?std::filesystem::path{}:canonical;
}
std::filesystem::path BackupPath(std::filesystem::path path){path+=".bak";return path;}
bool ReadDocument(const std::filesystem::path& path,std::string& text,std::string& error)
{
    std::error_code fileError;
    const auto size=std::filesystem::file_size(path,fileError);
    if(fileError||size>MaximumLayoutBytes){error="Editor.LayoutReadFailed";return false;}
    std::ifstream input(path,std::ios::binary);
    if(!input){error="Editor.LayoutReadFailed";return false;}
    text.assign(std::istreambuf_iterator<char>(input),{});
    if(input.bad()||text.size()!=size){error="Editor.LayoutReadFailed";return false;}
    return true;
}
bool ParseDocument(const std::string& text,std::array<EditorPanelState,EditorPanelCount>& panels,std::string& error)
{
    try
    {
        const auto json=Json::parse(text);
        if(!json.is_object()||!json.contains("schema")||!json.at("schema").is_number_integer())
        {error="Editor.LayoutInvalid";return false;}
        if(json.at("schema")!=EditorLayoutSchema){error="Editor.LayoutUnsupportedSchema";return false;}
        if(!json.at("panels").is_array()){error="Editor.LayoutInvalid";return false;}
        auto candidate=DefaultPanels();
        for(const auto& item:json.at("panels"))
        {
            EditorPanel panel;
            if(!ParseEditorPanelId(item.at("id").get<std::string>(),panel))continue;
            if(!item.at("visible").is_boolean()){error="Editor.LayoutInvalid";return false;}
            auto& state=candidate[Index(panel)];
            state.visible=item.at("visible").get<bool>();
            if(item.contains("geometry"))
            {
                const auto& geometry=item.at("geometry");
                for(const auto* field:{"x","y","width","height"})
                    if(!geometry.at(field).is_number()){error="Editor.LayoutInvalid";return false;}
                const EditorPanelGeometry value{geometry.at("x").get<float>(),geometry.at("y").get<float>(),
                    geometry.at("width").get<float>(),geometry.at("height").get<float>()};
                if(!ValidGeometry(value)){error="Editor.LayoutInvalid";return false;}
                state.geometry=value;
            }
        }
        if(std::none_of(candidate.begin(),candidate.end(),[](const auto& panel){return panel.visible;}))
        {error="Editor.LayoutInvalid";return false;}
        panels=std::move(candidate);
        return true;
    }
    catch(const std::exception&)
    {
        error="Editor.LayoutInvalid";return false;
    }
}
bool WriteDocument(const std::filesystem::path& path,const std::string& text)
{return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
}
const char* EditorPanelId(EditorPanel panel)
{
    switch(panel)
    {
    case EditorPanel::Hierarchy:return "hierarchy";
    case EditorPanel::Inspector:return "inspector";
    case EditorPanel::Assets:return "assets";
    case EditorPanel::MaterialEditor:return "material";
    case EditorPanel::Viewport:return "viewport";
    case EditorPanel::UiDesigner:return "ui-designer";
    case EditorPanel::ProjectSettings:return "project-settings";
    case EditorPanel::BuildExport:return "build-export";
    case EditorPanel::Console:return "console";
    }
    return "unknown";
}
bool ParseEditorPanelId(std::string_view id,EditorPanel& output)
{
    for(std::size_t index=0;index<EditorPanelCount;++index)
    {
        const auto panel=static_cast<EditorPanel>(index);
        if(id==EditorPanelId(panel)){output=panel;return true;}
    }
    return false;
}
EditorLayout::EditorLayout():panels_(DefaultPanels()){}
bool EditorLayout::Visible(EditorPanel panel) const{return ValidPanel(panel)&&panels_[Index(panel)].visible;}
void EditorLayout::SetVisible(EditorPanel panel,bool visible){if(ValidPanel(panel))panels_[Index(panel)].visible=visible;}
std::size_t EditorLayout::VisibleCount() const
{
    std::size_t count=0;
    for(const auto& panel:panels_)if(panel.visible)++count;
    return count;
}
bool EditorLayout::CaptureGeometry(EditorPanel panel,float x,float y,float width,float height,float viewportWidth,float viewportHeight)
{
    if(!ValidPanel(panel)||!ValidGeometry({x,y,width,height})
        ||!std::isfinite(viewportWidth)||!std::isfinite(viewportHeight)||viewportWidth<=0||viewportHeight<=0)return false;
    const EditorPanelGeometry value{x/viewportWidth,y/viewportHeight,width/viewportWidth,height/viewportHeight};
    if(!ValidGeometry(value))return false;
    auto& stored=panels_[Index(panel)].geometry;
    if(stored&&SameGeometry(*stored,value))return false;
    stored=value;
    return true;
}
std::optional<EditorPanelGeometry> EditorLayout::Geometry(EditorPanel panel,float viewportWidth,float viewportHeight,float uiScale) const
{
    if(!ValidPanel(panel)||!std::isfinite(viewportWidth)||!std::isfinite(viewportHeight)||!std::isfinite(uiScale)
        ||viewportWidth<=0||viewportHeight<=0||uiScale<=0)return {};
    const auto& geometry=panels_[Index(panel)].geometry;
    if(!geometry)return {};
    // 归一化位置不依赖旧显示器；顶部留出菜单，整个标题栏和面板都回到当前客户区。
    const double width=viewportWidth,height=viewportHeight,scale=uiScale;
    const double minimumWidth=std::min(width,160.0*scale);
    const double minimumHeight=std::min(height,96.0*scale);
    const double top=std::min(24.0*scale,std::max(0.0,height-minimumHeight));
    const double panelWidth=std::clamp(double(geometry->width)*width,minimumWidth,width);
    const double panelHeight=std::clamp(double(geometry->height)*height,minimumHeight,height-top);
    const double x=std::clamp(double(geometry->x)*width,0.0,width-panelWidth);
    const double y=std::clamp(double(geometry->y)*height,top,height-panelHeight);
    return EditorPanelGeometry{float(x),float(y),float(panelWidth),float(panelHeight)};
}
void EditorLayout::ResetDefaults()
{
    panels_=DefaultPanels();
    resetRequested_=protectedReplaceable_;
    lastError_.clear();
}
std::filesystem::path EditorLayout::DefaultPath()
{
    // 开发构建沿用仓库 out/Editor/User；便携包写入当前用户的 LOCALAPPDATA。
    return EditorSessionStore::UserDataDirectory()/"layout.json";
}
bool EditorLayout::Load(const std::filesystem::path& path,std::string& error)
{
    error.clear();lastError_.clear();
    panels_=DefaultPanels();protectedPath_.clear();protectedText_.clear();protectedReplaceable_=resetRequested_=false;
    std::error_code fileError;
    const bool exists=std::filesystem::exists(path,fileError);
    if(!exists&&!fileError)return true;
    std::string text;
    const bool read=!fileError&&ReadDocument(path,text,error);
    if(read&&ParseDocument(text,panels_,error))return true;
    if(fileError)error="Editor.LayoutReadFailed";
    lastError_=error;
    protectedPath_=PathIdentity(path);protectedText_=text;
    protectedReplaceable_=read&&error!="Editor.LayoutUnsupportedSchema";
    // 未来 schema 不是损坏，不降级到旧备份，也不授权显式重置覆盖。
    if(error=="Editor.LayoutUnsupportedSchema")return false;
    std::string backupText,backupError;
    auto backupPanels=DefaultPanels();
    if(ReadDocument(BackupPath(path),backupText,backupError)&&ParseDocument(backupText,backupPanels,backupError))
    {
        panels_=std::move(backupPanels);
        lastError_=error="Editor.LayoutRecoveredBackup";
        return true;
    }
    return false;
}
bool EditorLayout::Save(const std::filesystem::path& path,std::string& error) const
{
    error.clear();
    if(VisibleCount()==0){error="Editor.LayoutInvalid";return false;}
    Json panels=Json::array();
    for(std::size_t index=0;index<EditorPanelCount;++index)
    {
        Json item{{"id",EditorPanelId(static_cast<EditorPanel>(index))},{"visible",panels_[index].visible}};
        if(const auto& geometry=panels_[index].geometry)
            item["geometry"]={{"x",geometry->x},{"y",geometry->y},{"width",geometry->width},{"height",geometry->height}};
        panels.push_back(std::move(item));
    }
    const Json document{{"schema",EditorLayoutSchema},{"panels",panels}};
    std::error_code fileError;
    const bool exists=std::filesystem::exists(path,fileError);
    if(fileError){error="Editor.LayoutReadFailed";return false;}
    if(exists)
    {
        std::string previous,previousError;
        if(!ReadDocument(path,previous,previousError)){error=previousError;return false;}
        auto previousPanels=DefaultPanels();
        if(ParseDocument(previous,previousPanels,previousError))
        {
            // 只让有效的主文件进入备份，备份失败时保留原主文件。
            if(!WriteDocument(BackupPath(path),previous)){error="Editor.LayoutBackupWriteFailed";return false;}
        }
        else if(previousError=="Editor.LayoutUnsupportedSchema"||!resetRequested_||!protectedReplaceable_
            ||protectedPath_.empty()||PathIdentity(path)!=protectedPath_||previous!=protectedText_)
        {error="Editor.LayoutProtected";return false;}
    }
    if(!WriteDocument(path,document.dump(2)+"\n")){error="Editor.LayoutWriteFailed";return false;}
    resetRequested_=false;
    return true;
}
}
