#include "ImGuiEditor.h"
#include "EditorTheme.h"
#include "PropertyWidgets.h"
#include "AssetImportService.h"
#include "EditorSessionStore.h"
#include "EditorProcessRunner.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "ResourceLayer/Assets/MaterialMetadata.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include <imgui/imgui_internal.h>
#include "ResourceLayer/Assets/AssetIO.h"
#include <Windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace se
{
namespace
{
struct ContextScope
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* next) { ImGui::SetCurrentContext(next); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};
Vec3 Axis(GizmoAxis axis)
{ return axis == GizmoAxis::X ? Vec3{1,0,0} : axis == GizmoAxis::Y ? Vec3{0,1,0} : Vec3{0,0,1}; }
std::optional<Vec2> Project(Vec3 point, const Mat4& matrix, EditorRect rect)
{
    const auto clip = matrix * Vec4{point.x,point.y,point.z,1};
    if (!std::isfinite(clip.w) || clip.w <= 0.0001f) return {};
    const Vec2 screen{rect.x + (clip.x / clip.w * 0.5f + 0.5f) * rect.width,
        rect.y + (0.5f - clip.y / clip.w * 0.5f) * rect.height};
    if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) return {};
    return screen;
}
float DistanceToSegment(Vec2 p, Vec2 a, Vec2 b)
{
    const float x = b.x-a.x, y = b.y-a.y, length = x*x+y*y;
    const float t = length > 0 ? std::clamp(((p.x-a.x)*x+(p.y-a.y)*y)/length,0.0f,1.0f) : 0;
    return std::hypot(p.x-a.x-x*t,p.y-a.y-y*t);
}
template<std::size_t N> void CopyText(std::array<char,N>& buffer, const std::string& text)
{
    const auto size = std::min(text.size(), N-1);
    std::memcpy(buffer.data(),text.data(),size); buffer[size]=0;
}
bool AcceptAsset(AssetId& id)
{
    if (!ImGui::BeginDragDropTarget()) return false;
    bool accepted = false;
    if (const auto* payload = ImGui::AcceptDragDropPayload("SE_ASSET"))
        if (payload->DataSize == 32) { id.assign(static_cast<const char*>(payload->Data),32); accepted=true; }
    ImGui::EndDragDropTarget(); return accepted;
}
bool AcceptEntity(StableEntityId& id)
{
    if (!ImGui::BeginDragDropTarget()) return false;
    bool accepted = false;
    if (const auto* payload = ImGui::AcceptDragDropPayload("SE_ENTITY"))
        if (payload->DataSize == sizeof(id)) { std::memcpy(&id,payload->Data,sizeof(id)); accepted=true; }
    ImGui::EndDragDropTarget(); return accepted;
}
std::optional<double> AxisParameter(const CameraSnapshot& camera,Vec2 mouse,Vec3 origin,Vec3 axis)
{
    const auto ray=ViewportPointToRay(camera,mouse,true);if(!ray)return {};
    const auto dot=[](Vec3 a,Vec3 b){return double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z;};
    const auto offset=origin-ray->origin;const auto a=dot(axis,axis),b=dot(axis,ray->direction),c=dot(ray->direction,ray->direction);
    const auto denominator=a*c-b*b;if(!std::isfinite(denominator)||denominator<1e-5)return {};
    const auto result=(b*dot(ray->direction,offset)-c*dot(axis,offset))/denominator;
    if(!std::isfinite(result)||std::abs(result)>(std::numeric_limits<float>::max)())return {};return result;
}
}

Vec3 EditorGizmoAxis(const World& world,EntityHandle entity,GizmoAxis axis,GizmoSpace space)
{
    if(!world.IsAlive(entity)||static_cast<unsigned>(axis)>2||static_cast<unsigned>(space)>1)return {};
    if(space==GizmoSpace::World)return Axis(axis);
    Quat orientation;
    for(auto cursor=entity;cursor;cursor=world.Get(cursor)->parent)orientation=Normalize(world.GetTransform(cursor)->rotation*orientation);
    return Normalize(Rotate(orientation,Axis(axis)));
}
bool EditorGizmo::Begin(EditorSession& editor, GizmoMode mode, GizmoAxis axis,
    Vec2 mouse,Vec2 screenDirection,float pixelsPerUnit,GizmoSpace space,const CameraSnapshot* camera)
{
    if (active_ || editor.IsPlaying() || !std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0
        || !std::isfinite(mouse.x) || !std::isfinite(mouse.y)
        || static_cast<unsigned>(mode)>2 || static_cast<unsigned>(axis)>2||static_cast<unsigned>(space)>1)return Fail("Editor.InvalidGizmo");
    const float length = std::hypot(screenDirection.x,screenDirection.y);
    if (!std::isfinite(length) || length < 0.001f)return Fail("Editor.InvalidGizmo");
    const auto& world = editor.EditWorld();
    const auto entity = world.FindByStableId(editor.Selected());
    const auto* transform = world.GetTransform(entity);
    if (!transform)return Fail("Editor.InvalidSelection");
    const auto* record = world.Get(entity);
    const auto inverse = record->parent ? Inverse(world.GetWorldMatrix(record->parent)) : std::optional<Mat4>(Mat4{});
    if(!inverse)return Fail("Editor.TransformNotRepresentable");
    const auto direction=EditorGizmoAxis(world,entity,axis,space);const auto initialWorld=world.GetWorldMatrix(entity);
    for(const auto value:initialWorld.values)if(!IsFinite(value))return Fail("Editor.TransformNotRepresentable");
    std::optional<double> parameter;
    if(camera&&mode==GizmoMode::Translate){parameter=AxisParameter(*camera,mouse,TransformPoint(initialWorld,{}),direction);if(!parameter)return Fail("Editor.InvalidGizmo");}
    if(!editor.BeginEdit("Editor.Transform")){error_=editor.LastError();return false;}
    initial_ = *transform; parentInverse_ = *inverse; entity_ = record->stableId;
    mode_=mode; axis_=axis; mouse_=mouse;space_=space;axisDirection_=direction;initialWorld_=initialWorld;
    camera_=camera?std::optional<CameraSnapshot>(*camera):std::nullopt;initialAxisParameter_=parameter.value_or(0);
    direction_={screenDirection.x/length,screenDirection.y/length}; pixelsPerUnit_=pixelsPerUnit;
    active_=true;error_.clear();return true;
}
bool EditorGizmo::ProjectionMatches(const std::optional<CameraSnapshot>& camera) const
{
    if(!camera_)return true;if(!camera)return false;
    const auto a=camera_->viewport,b=camera->viewport;
    return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height&&camera_->viewProjection.values==camera->viewProjection.values;
}
bool EditorGizmo::Update(EditorSession& editor, Vec2 mouse)
{
    if(!active_||!std::isfinite(mouse.x)||!std::isfinite(mouse.y))return Fail("Editor.InvalidGizmo");
    float amount=((mouse.x-mouse_.x)*direction_.x+(mouse.y-mouse_.y)*direction_.y)/pixelsPerUnit_;
    if(mode_==GizmoMode::Translate&&camera_)
    {
        const auto parameter=AxisParameter(*camera_,mouse,TransformPoint(initialWorld_,{}),axisDirection_);
        if(!parameter)return Fail("Editor.InvalidGizmo");amount=static_cast<float>(*parameter-initialAxisParameter_);
    }
    if(!IsFinite(amount))return Fail("Editor.InvalidGizmo");
    Transform value=initial_;
    if(mode_==GizmoMode::Translate)value.position+=TransformVector(parentInverse_,axisDirection_*amount);
    else if(space_==GizmoSpace::Local&&mode_==GizmoMode::Rotate)value.rotation=Normalize(initial_.rotation*AxisAngle(Axis(axis_),amount));
    else if(space_==GizmoSpace::Local)
    {
        const float factor=std::max(0.001f,1.0f+amount);
        if(axis_==GizmoAxis::X)value.scale.x*=factor;
        if(axis_==GizmoAxis::Y)value.scale.y*=factor;
        if(axis_==GizmoAxis::Z)value.scale.z*=factor;
    }
    else
    {
        Vec3 factors{1,1,1};const float factor=std::max(.001f,1+amount);
        if(axis_==GizmoAxis::X)factors.x=factor;if(axis_==GizmoAxis::Y)factors.y=factor;if(axis_==GizmoAxis::Z)factors.z=factor;
        const auto operation=mode_==GizmoMode::Rotate?Rotation(AxisAngle(Axis(axis_),amount)):Scaling(factors);
        auto desired=operation*initialWorld_;for(size_t row=0;row<3;++row)desired(row,3)=initialWorld_(row,3);
        if(!TryDecomposeTrs(parentInverse_*desired,value))return Fail("Editor.TransformNotRepresentable");
    }
    if(!editor.SetTransform(entity_,value)){error_=editor.LastError();return false;}error_.clear();return true;
}
bool EditorGizmo::End(EditorSession& editor, bool commit)
{
    if(!active_)return false;
    const bool result=commit?editor.CommitEdit():editor.CancelEdit();
    if(result)active_=false; return result;
}

ImGuiEditor::ImGuiEditor()
{
    auto* previous=ImGui::GetCurrentContext();
    context_=ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly=true; // 场景 Image/自绘手柄不应同时启动窗口拖动。
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_DockingEnable;
    ApplyEditorTheme(ImGui::GetStyle());
    CopyText(scenePath_,"Resources/Scenes/Untitled.scene.json");
    CopyText(sceneSavePath_,"Resources/Scenes/Untitled.scene.json");
    CopyText(uiPath_,"Resources/UI/Untitled.ui.json");
    CopyText(materialPath_,"Materials/Default.material.json");
    userOperationDirectory_=EditorSessionStore::UserDataDirectory()/"operations";
    ImGui::SetCurrentContext(previous);
}
ImGuiEditor::~ImGuiEditor()
{
    // 工作线程可能仍在使用本对象的 runner 闭包；先取消并等待结束后再销毁 context。
    if(exportJob_){exportJob_->Cancel();exportJob_->Join();}
    if(!layoutPath_.empty())SaveLayout();
    if(context_)SaveDockLayout();
    ImGui::DestroyContext(context_);
}
bool ImGuiEditor::LoadFont(const std::filesystem::path& fontFile,float pixelSize)
{
    if(!std::isfinite(pixelSize)||pixelSize<6||pixelSize>96){errorKey_="EditorUi.FontLoadFailed";return false;}
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(fontFile,bytes)||bytes.empty()||bytes.size()>static_cast<std::size_t>(INT_MAX))
    {errorKey_="EditorUi.FontLoadFailed";return false;}
    ContextScope context(context_);
    auto* fonts=ImGui::GetIO().Fonts;
    fonts->Clear(); fontBytes_=std::move(bytes);
    ImFontConfig config; config.FontDataOwnedByAtlas=false;
    const bool loaded=fonts->AddFontFromMemoryTTF(fontBytes_.data(),static_cast<int>(fontBytes_.size()),pixelSize,&config,fonts->GetGlyphRangesChineseFull())!=nullptr;
    if(!loaded)errorKey_="EditorUi.FontLoadFailed";
    else if(errorKey_=="EditorUi.FontLoadFailed")errorKey_.clear();
    return loaded;
}
void ImGuiEditor::FontAtlasRgba(const unsigned char*& pixels,int& width,int& height)
{
    ContextScope context(context_); unsigned char* writable=nullptr;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&writable,&width,&height); pixels=writable;
}
void ImGuiEditor::SetFontTexture(ImTextureID texture)
{ ContextScope context(context_); ImGui::GetIO().Fonts->SetTexID(texture); }
void ImGuiEditor::SetDocumentPaths(const std::filesystem::path& scene,const std::filesystem::path& ui)
{CopyText(scenePath_,asset_io::Utf8(scene));CopyText(sceneSavePath_,asset_io::Utf8(scene));CopyText(uiPath_,asset_io::Utf8(ui));}
std::string ImGuiEditor::Label(const Localization& text,const char* key) const
{ return text.GetText(key)+"###"+key; }
void ImGuiEditor::SetLayoutPath(std::filesystem::path path)
{
    layoutPath_=std::move(path);layoutApply_=true;
    if(layoutPath_.empty())return;
    std::string error;layout_.Load(layoutPath_,error);
    if(!error.empty())LogConsole(1,"Editor.LayoutLoadWarning",error);
    std::error_code existsError;
    if(!std::filesystem::exists(layoutPath_,existsError)&&!existsError)
    {std::string saveError;if(!layout_.Save(layoutPath_,saveError))LogConsole(1,"Editor.LayoutSaveFailed",saveError);}
    showHierarchy_=layout_.Visible(EditorPanel::Hierarchy);showInspector_=layout_.Visible(EditorPanel::Inspector);
    showAssets_=layout_.Visible(EditorPanel::Assets);showViewport_=layout_.Visible(EditorPanel::Viewport);
    showMaterial_=layout_.Visible(EditorPanel::MaterialEditor);showUi_=layout_.Visible(EditorPanel::UiDesigner);
    showProjectSettings_=layout_.Visible(EditorPanel::ProjectSettings);showBuild_=layout_.Visible(EditorPanel::BuildExport);
    showConsole_=layout_.Visible(EditorPanel::Console);
}
void ImGuiEditor::SetUiScale(float userScale,float dpiScale)
{
    if(!std::isfinite(userScale)||!std::isfinite(dpiScale)||userScale<0.5f||userScale>3||dpiScale<0.5f||dpiScale>4)return;
    ContextScope context(context_);auto& style=ImGui::GetStyle();
    style.ScaleAllSizes((userScale*dpiScale)/(uiScale_*dpiScale_));
    uiScale_=userScale;dpiScale_=dpiScale;style.FontScaleMain=userScale;style.FontScaleDpi=dpiScale;layoutApply_=true;
}
void ImGuiEditor::SaveLayout()
{
    if(layoutPath_.empty())return;
    layout_.SetVisible(EditorPanel::Hierarchy,showHierarchy_);layout_.SetVisible(EditorPanel::Inspector,showInspector_);
    layout_.SetVisible(EditorPanel::Assets,showAssets_);layout_.SetVisible(EditorPanel::Viewport,showViewport_);
    layout_.SetVisible(EditorPanel::MaterialEditor,showMaterial_);layout_.SetVisible(EditorPanel::UiDesigner,showUi_);
    layout_.SetVisible(EditorPanel::ProjectSettings,showProjectSettings_);layout_.SetVisible(EditorPanel::BuildExport,showBuild_);
    layout_.SetVisible(EditorPanel::Console,showConsole_);
    std::string error;
    if(!layout_.Save(layoutPath_,error))LogConsole(1,"Editor.LayoutSaveFailed",error);
    layoutDirty_=false;
}

void ImGuiEditor::PreparePanel(EditorPanel panel,float x,float y,float width,float height)
{
    if(ImGui::GetIO().ConfigFlags&ImGuiConfigFlags_DockingEnable)return;
    const auto display=ImGui::GetIO().DisplaySize;
    const auto& panelStyle=ImGui::GetStyle();
    const float top=std::min(display.y-1,ImGui::GetFrameHeight()+ImGui::GetFrameHeightWithSpacing()+2*panelStyle.WindowPadding.y+panelStyle.ScrollbarSize);
    auto geometry=layout_.Geometry(panel,display.x,display.y,uiScale_*dpiScale_);
    if(geometry)
    {
        geometry->height=std::min(geometry->height,display.y-top);geometry->y=std::clamp(geometry->y,top,std::max(top,display.y-geometry->height));
        ImGui::SetNextWindowPos({geometry->x,geometry->y},layoutApply_?ImGuiCond_Always:ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({geometry->width,geometry->height},layoutApply_?ImGuiCond_Always:ImGuiCond_FirstUseEver);
    }
    else
    {
        const float scaleX=display.x/1280.0f,scaleY=(display.y-top)/(760.0f-30.0f);
        const float w=std::min(display.x,std::max(160.0f,width*scaleX)),h=std::min(display.y-top,std::max(96.0f,height*scaleY));
        ImGui::SetNextWindowPos({std::clamp(x*scaleX,0.0f,std::max(0.0f,display.x-w)),std::clamp(top+std::max(0.0f,y-30.0f)*scaleY,top,std::max(top,display.y-h))},layoutApply_&&!layoutPath_.empty()?ImGuiCond_Always:ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({w,h},layoutApply_&&!layoutPath_.empty()?ImGuiCond_Always:ImGuiCond_FirstUseEver);
    }
}
void ImGuiEditor::RecordLayout()
{
    if(layoutPath_.empty())return;
    const char* names[]={"###EditorUi.Hierarchy","###EditorUi.Inspector","###EditorUi.Assets","###EditorUi.MaterialEditor","###EditorUi.Viewport","###EditorUi.UiDesigner","###EditorUi.ProjectSettings","###EditorUi.BuildExport","###EditorUi.Console"};
    const bool visible[]={showHierarchy_,showInspector_,showAssets_,showMaterial_,showViewport_,showUi_,showProjectSettings_,showBuild_,showConsole_};
    const auto display=ImGui::GetIO().DisplaySize;
    const bool docking=(ImGui::GetIO().ConfigFlags&ImGuiConfigFlags_DockingEnable)!=0;
    bool changed=false;
    for(std::size_t index=0;index<EditorPanelCount;++index)
    {
        const auto panel=static_cast<EditorPanel>(index);
        if(layout_.Visible(panel)!=visible[index]){layout_.SetVisible(panel,visible[index]);changed=true;}
        if(docking)continue;
        auto* window=ImGui::FindWindowByID(ImHashStr(names[index]));
        if(window&&window->Active&&!window->Collapsed)
            changed=layout_.CaptureGeometry(panel,window->Pos.x,window->Pos.y,window->Size.x,window->Size.y,display.x,display.y)||changed;
    }
    if(changed){layoutDirty_=true;layoutChangedAt_=ImGui::GetTime();}
    if(layoutDirty_&&!ImGui::IsMouseDown(ImGuiMouseButton_Left)&&ImGui::GetTime()-layoutChangedAt_>=1.0)SaveLayout();
}

EditorFrameResult ImGuiEditor::Draw(EditorSession& session,AssetStore& assets,const Localization& text,
    const EditorInputFrame& input,const EditorViewport& viewport,UiTree* ui)
{
    ContextScope context(context_);
    auto& io=ImGui::GetIO();
    if(!input.usePlatformInput)
    {
    io.DisplaySize={std::max(1.0f,input.displaySize.x),std::max(1.0f,input.displaySize.y)};
    io.DeltaTime=std::isfinite(input.deltaSeconds)?std::clamp(input.deltaSeconds,0.0001f,1.0f):1.0f/60;
    io.AddFocusEvent(input.focused);
    io.AddMousePosEvent(input.mousePosition.x,input.mousePosition.y);
    for(int button=0;button<5;++button)io.AddMouseButtonEvent(button,input.focused&&input.mouseButtons[static_cast<std::size_t>(button)]);
    io.AddMouseWheelEvent(input.wheelX,input.wheelY);
    for(const auto& [key,down]:input.keyEvents)io.AddKeyEvent(key,input.focused&&down);
    if(input.focused&&!input.textUtf8.empty())io.AddInputCharactersUTF8(input.textUtf8.c_str());
    }
    if(!io.Fonts->IsBuilt()){const unsigned char* pixels;int width,height;FontAtlasRgba(pixels,width,height);}
    ImGui::NewFrame();
    if(!layoutPath_.empty()&&(layoutViewport_.x!=io.DisplaySize.x||layoutViewport_.y!=io.DisplaySize.y))layoutApply_=true;
    layoutViewport_=io.DisplaySize;
    if(input.usePlatformInput?io.AppFocusLost:!input.focused)CancelInteractions(session,ui);
    PollExport(text);
    SyncDocumentRegistry(session,ui);
    if(requestClose_)
    {
        if(AssetOperationBusy()){errorKey_="Editor.AssetOperationBusy";requestClose_=false;}
        else if(ExportBusy()){RequestCancelExport();errorKey_="Editor.ExportCancelled";requestClose_=false;LogConsole(1,"Editor.ExportCancelled");}
        else{CancelInteractions(session,ui);requestClose_=false;saveAsOpen_=saveAsRequested_=materialOverwrite_=continueCloseAfterMaterial_=false;files_.RequestClose(session,OtherDocumentsDirty());}
    }
    if(ui)BindUi(*ui);
    Menu(session,assets,text,ui);
    DrawDockSpace(text);
    ResolvePendingReference(session);
    if(!showMaterial_&&material_.Editing()){material_.CancelEdit();materialField_.clear();}
    if(showHierarchy_)Hierarchy(session,text);
    if(showInspector_)Inspector(session,assets,text);
    if(showAssets_)Assets(session,assets,text);
    if(showMaterial_){ImGui::BeginDisabled(session.IsPlaying());MaterialEditor(session,assets,text);ImGui::EndDisabled();}
    EditorFrameResult result;
    if(ui&&showUi_&&!session.IsPlaying())
    {
        result.viewport=UiDesigner(*ui,assets,text,viewport);result.uiCanvas=result.viewport.width>0&&result.viewport.height>0;
    }
    if(!result.uiCanvas)
    {
        if(ui&&uiDragging_){uiCanvas_.End(*ui,false);EndUiEdit(*ui,false);uiDragging_=false;}
        if(showViewport_)result.viewport=Viewport(session,assets,text,viewport);
        else{result.viewport={0,0,0,0};if(gizmo_.Active())gizmo_.End(session,false);}
    }
    else if(gizmo_.Active())gizmo_.End(session,false);
    for(auto& action:pending_)action(); pending_.clear();
    // 项目/构建/控制台在视口之后绘制，避免新增浮窗被场景视口遮住；不参与视口命中。
    ProjectSettingsPanel(text);
    BuildExportPanel(session,assets,text,ui);
    ConsolePanel(text);
    if(showAbout_)AboutPanel(text);
    if(showDiagnostics_)DiagnosticsPanel(text);
    if(showPreferences_)PreferencesPanel(text);
    if(showBuildDiagnostics_)BuildDiagnosticsPanel(text);
    ImportDialogs(session,assets,text);
    RecoveryDialogs(session,assets,ui,text);
    if(result.uiCanvas&&ui)
    {uiCanvas_.Prepare(*ui,{result.viewport.x,result.viewport.y,result.viewport.width,result.viewport.height});result.uiPreview=uiCanvas_.Visuals(*ui);}
    ApplySessionDocumentFocus(session);
    RecordLayout();layoutApply_=false;
    ImGui::Render(); result.drawData=ImGui::GetDrawData();
    result.captureMouse=io.WantCaptureMouse; result.captureKeyboard=io.WantCaptureKeyboard;
    return result;
}

void ImGuiEditor::CancelInteractions(EditorSession& session,UiTree* ui)
{
    if(gizmo_.Active())gizmo_.End(session,false);
    if(inspectorEditing_){session.CancelEdit();inspectorEditing_=false;inspectorField_.clear();}
    if(material_.Editing()){material_.CancelEdit();materialField_.clear();}
    if(ui&&!uiTransaction_.empty())EndUiEdit(*ui,false);
    uiCanvas_.Reset();uiDragging_=false;
}
EditorCommandOutcome ImGuiEditor::ExecuteCommand(std::string_view id)
{
    auto outcome=commands_.Execute(id,{});
    if(outcome.state!=EditorCommandResult::Succeeded)errorKey_=outcome.diagnosticKey;
    return outcome;
}

void ImGuiEditor::Menu(EditorSession& editor,AssetStore& assets,const Localization& text,UiTree* ui)
{
    const bool materialContext=documentContext_==DocumentContext::Material&&showMaterial_&&materialReady_;
    const bool uiContext=documentContext_==DocumentContext::Ui&&ui&&uiOwner_==ui&&showUi_;
    const auto editable=[this,&editor](std::string& reason){
        if(editor.IsPlaying()){reason="Editor.EditWhilePlaying";return false;}
        if(gizmo_.Active()||inspectorEditing_||material_.Editing()||!uiTransaction_.empty()){reason="Editor.TransactionActive";return false;}
        if(FileDialogActive()){reason="Editor.CommandBlocked";return false;}return true;
    };
    const auto undo=[this,&editor,materialContext,uiContext]{const bool ok=materialContext?material_.Undo():uiContext?UiUndo(*uiOwner_):editor.Undo();return ok?EditorCommandOutcome::Succeeded():EditorCommandOutcome::Failed(materialContext?material_.LastError():uiContext?errorKey_:editor.LastError());};
    const auto redo=[this,&editor,materialContext,uiContext]{const bool ok=materialContext?material_.Redo():uiContext?UiRedo(*uiOwner_):editor.Redo();return ok?EditorCommandOutcome::Succeeded():EditorCommandOutcome::Failed(materialContext?material_.LastError():uiContext?errorKey_:editor.LastError());};
    const auto save=[this,&editor,&assets,materialContext,uiContext]{
        if(documentContext_==DocumentContext::ProjectSettings||documentContext_==DocumentContext::BuildSettings)
            return SaveSettings(documentContext_==DocumentContext::ProjectSettings)?EditorCommandOutcome::Succeeded():EditorCommandOutcome::Failed(errorKey_);
        if(materialContext)return SaveMaterial(assets)?EditorCommandOutcome::Succeeded():EditorCommandOutcome::Failed(errorKey_);
        if(uiContext)return SaveUi(*uiOwner_,asset_io::FromUtf8(uiPath_.data()))?EditorCommandOutcome::Succeeded():EditorCommandOutcome::Failed(errorKey_);
        const auto result=files_.SaveCurrent(editor,asset_io::FromUtf8(sceneSavePath_.data()));
        if(result==FileActionResult::Waiting)return EditorCommandOutcome{EditorCommandResult::Deferred,"Editor.SaveAllPending",{}};
        return result==FileActionResult::Done?EditorCommandOutcome::Succeeded():EditorCommandOutcome::Failed(files_.LastError());
    };
    commands_.Register("editor.undo",[undo](const EditorCommandContext&){return undo();},editable);
    commands_.Register("editor.redo",[redo](const EditorCommandContext&){return redo();},editable);
    const auto saveAll=[this,&editor,&assets,ui]{return SaveAllDocuments(editor,assets,ui);};
    commands_.Register("editor.save",[save](const EditorCommandContext&){return save();},editable);
    commands_.Register("editor.save-all",[saveAll](const EditorCommandContext&){return saveAll()?EditorCommandOutcome::Succeeded():EditorCommandOutcome::Failed("Editor.SaveAllFailed");},editable);
    commands_.Register("editor.compile",[this,saveAll](const EditorCommandContext&){
        if(!saveAll())return EditorCommandOutcome::Failed(errorKey_);
        recompileRequested_=true;RequestClose();return EditorCommandOutcome::Succeeded();
    },[this,editable](std::string& reason){if(restartRequestPath_.empty()){reason="Editor.BuildControlInvalid";return false;}if(ExportBusy()){reason="Editor.ExportBusy";return false;}return editable(reason);});
    commands_.Register("editor.use-last-build",[this](const EditorCommandContext&){allowStaleBuild_=true;return EditorCommandOutcome::Succeeded();},
        [this,editable](std::string& reason){if(!buildStale_||!buildHasCandidate_){reason="Editor.BuildRequired";return false;}return editable(reason);});
    const auto play=[this,&editor,&assets,ui]{
        std::string error;
        if(!ValidateBuildSources())return EditorCommandOutcome::Rejected("Editor.BuildRequired");
        std::vector<EditorAssetIssue> issues;
        std::vector<EditorAssetDocument> documents;
        if(!CaptureEditingDocuments(editor,assets,ui,documents))return EditorCommandOutcome::Failed(errorKey_);
        if(!EditorAssetActions::ValidatePlayAssets(editor.EditWorld(),ui,assets,documents,issues))
        {
            for(const auto& issue:issues)LogConsole(2,issue.key,issue.detail);
            if(!issues.empty())LocateAssetReference(editor,assets,issues.front().reference);
            return EditorCommandOutcome::Failed(issues.empty()?"Assets.PlayValidationFailed":issues.front().key);
        }
        if(!playback_.Play(error))return EditorCommandOutcome::Rejected(error);
        std::string validation;
        if(!editor.ValidatePlay(validation)){playback_.Failed(error);return EditorCommandOutcome::Failed(validation);}
        if(!editor.StartPlay()){const auto failure=editor.LastError();playback_.Failed(error);return EditorCommandOutcome::Failed(failure);}
        if(!playback_.Prepared(error)){editor.StopPlay();playback_.Reset();return EditorCommandOutcome::Failed(error);}
        editor.SetPlayPaused(false);errorKey_.clear();return EditorCommandOutcome::Succeeded();
    };
    const auto pause=[this,&editor]{
        if(!editor.IsPlaying())return EditorCommandOutcome::Rejected("Editor.PlaybackInvalidState");
        std::string error;if(!playback_.Pause(error))return EditorCommandOutcome::Rejected(error);
        if(!editor.SetPlayPaused(true))return EditorCommandOutcome::Failed(editor.LastError());
        return EditorCommandOutcome::Succeeded();
    };
    const auto resume=[this,&editor]{
        if(!editor.IsPlaying())return EditorCommandOutcome::Rejected("Editor.PlaybackInvalidState");
        std::string error;if(!playback_.Resume(error))return EditorCommandOutcome::Rejected(error);
        if(!editor.SetPlayPaused(false))return EditorCommandOutcome::Failed(editor.LastError());
        return EditorCommandOutcome::Succeeded();
    };
    const auto step=[this,&editor]{
        if(!editor.IsPlaying()||!editor.PlayPaused())return EditorCommandOutcome::Rejected("Editor.PlaybackInvalidState");
        std::string error;if(!playback_.Step(error))return EditorCommandOutcome::Rejected(error);
        editor.RequestPlayStep();return EditorCommandOutcome::Succeeded();
    };
    const auto stop=[this,&editor]{
        if(!editor.IsPlaying())return EditorCommandOutcome::Rejected("Editor.PlaybackInvalidState");
        playback_.Reset();editor.StopPlay();return EditorCommandOutcome::Succeeded();
    };
    const auto executePlayback=[this](const char* id){const auto outcome=commands_.Execute(id,{});if(outcome.state!=EditorCommandResult::Succeeded)errorKey_=outcome.diagnosticKey.empty()?"Editor.PlaybackInvalidState":outcome.diagnosticKey;};
    commands_.Register("editor.play",[play](const EditorCommandContext&){return play();},editable);
    commands_.Register("editor.pause",[pause](const EditorCommandContext&){return pause();});
    commands_.Register("editor.resume",[resume](const EditorCommandContext&){return resume();});
    commands_.Register("editor.step",[step](const EditorCommandContext&){return step();});
    commands_.Register("editor.stop",[stop](const EditorCommandContext&){return stop();});
    if(ImGui::BeginMainMenuBar())
    {
        if(ImGui::BeginMenu(Label(text,"EditorUi.File").c_str()))
        {
            ImGui::InputText(Label(text,"EditorUi.ScenePath").c_str(),scenePath_.data(),scenePath_.size());
            ImGui::BeginDisabled(editor.IsPlaying()||gizmo_.Active()||inspectorEditing_||material_.Editing()||FileDialogActive());
            if(ImGui::MenuItem(Label(text,"EditorUi.Open").c_str()))
            {
                Queue([this,&editor]{documentContext_=DocumentContext::Scene;if(files_.RequestOpen(editor,asset_io::FromUtf8(scenePath_.data()))==FileActionResult::Failed)errorKey_=files_.LastError();});
            }
            if(ImGui::MenuItem(Label(text,"EditorUi.Save").c_str(),"Ctrl+S"))
                Queue([this]{commands_.Execute("editor.save",{});});
            if(ImGui::MenuItem(Label(text,"EditorUi.SaveAll").c_str(),"Ctrl+Shift+Alt+S"))
                Queue([this]{commands_.Execute("editor.save-all",{});});
            if(ImGui::MenuItem(Label(text,"EditorUi.SaveSceneAs").c_str()))saveAsRequested_=true;
            ImGui::EndDisabled();
            if(ImGui::MenuItem(Label(text,"EditorUi.CloseEditor").c_str(),nullptr,false,!ExportBusy()))RequestClose();
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu(Label(text,"EditorUi.Edit").c_str()))
        {
            const bool enabled=!editor.IsPlaying()&&!gizmo_.Active()&&!inspectorEditing_&&!material_.Editing()&&uiTransaction_.empty()&&!FileDialogActive();
            if(ImGui::MenuItem(Label(text,"EditorUi.Undo").c_str(),"Ctrl+Z",false,enabled&&(materialContext?material_.UndoCount():uiContext?uiUndo_.size():editor.UndoCount())>0))Queue([this]{commands_.Execute("editor.undo",{});});
            if(ImGui::MenuItem(Label(text,"EditorUi.Redo").c_str(),"Ctrl+Y",false,enabled&&(materialContext?material_.RedoCount():uiContext?uiRedo_.size():editor.RedoCount())>0))Queue([this]{commands_.Execute("editor.redo",{});});
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu(Label(text,"EditorUi.Project").c_str()))
        {
            if(ImGui::MenuItem(Label(text,"EditorUi.ProjectSettings").c_str()))
            {showProjectSettings_=true;SaveLayout();}
            if(ImGui::MenuItem(Label(text,"EditorUi.BuildExport").c_str()))
            {showBuild_=true;SaveLayout();}
            ImGui::Separator();
            if(ImGui::MenuItem(Label(text,"EditorUi.OpenProjectFolder").c_str()))OpenInExplorer(EditorRoot());
            if(ImGui::MenuItem(Label(text,"EditorUi.OpenLogDirectory").c_str()))
            {std::error_code error;std::filesystem::create_directories(userOperationDirectory_,error);OpenInExplorer(userOperationDirectory_);}
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu(Label(text,"EditorUi.Panels").c_str()))
        {bool changed=ImGui::MenuItem(Label(text,"EditorUi.MaterialEditor").c_str(),nullptr,&showMaterial_);changed|=ImGui::MenuItem(Label(text,"EditorUi.UiDesigner").c_str(),nullptr,&showUi_);ImGui::Separator();
             changed|=ImGui::MenuItem(Label(text,"EditorUi.Hierarchy").c_str(),nullptr,&showHierarchy_);changed|=ImGui::MenuItem(Label(text,"EditorUi.Inspector").c_str(),nullptr,&showInspector_);
             changed|=ImGui::MenuItem(Label(text,"EditorUi.Assets").c_str(),nullptr,&showAssets_);changed|=ImGui::MenuItem(Label(text,"EditorUi.Viewport").c_str(),nullptr,&showViewport_);
             changed|=ImGui::MenuItem(Label(text,"EditorUi.ProjectSettings").c_str(),nullptr,&showProjectSettings_);changed|=ImGui::MenuItem(Label(text,"EditorUi.BuildExport").c_str(),nullptr,&showBuild_);
             changed|=ImGui::MenuItem(Label(text,"EditorUi.Console").c_str(),nullptr,&showConsole_);
             if(changed)SaveLayout();ImGui::Separator();
             if(ImGui::MenuItem(Label(text,"EditorUi.ResetLayout").c_str())){layout_.ResetDefaults();layoutApply_=true;showHierarchy_=showInspector_=showAssets_=showViewport_=showConsole_=true;showMaterial_=showUi_=showProjectSettings_=showBuild_=false;SaveLayout();ResetDockLayout();}
             ImGui::Separator();ImGui::MenuItem(Label(text,"Editor.Preferences").c_str(),nullptr,&showPreferences_);
             ImGui::EndMenu();}
        if(ImGui::BeginMenu(Label(text,"EditorUi.Help").c_str()))
        {
            if(ImGui::MenuItem(Label(text,"EditorUi.Diagnostics").c_str())){showDiagnostics_=true;RunEnvironmentDiagnostics(text);}
            if(ImGui::MenuItem(Label(text,"EditorUi.OpenLogDirectory").c_str()))
            {std::error_code error;std::filesystem::create_directories(userOperationDirectory_,error);OpenInExplorer(userOperationDirectory_);}
            ImGui::Separator();
            if(ImGui::MenuItem(Label(text,"EditorUi.About").c_str()))showAbout_=true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
        const auto& toolbarStyle=ImGui::GetStyle();
        ImGui::SetNextWindowPos({0,ImGui::GetFrameHeight()});
        ImGui::SetNextWindowSize({ImGui::GetIO().DisplaySize.x,ImGui::GetFrameHeightWithSpacing()+2*toolbarStyle.WindowPadding.y+toolbarStyle.ScrollbarSize});
        ImGui::Begin("###EditorUi.Toolbar",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::BeginDisabled(gizmo_.Active()||inspectorEditing_||material_.Editing()||!uiTransaction_.empty()||FileDialogActive());
        if(!editor.IsPlaying())
        {
            if(ImGui::Button(Label(text,"EditorUi.Play").c_str()))Queue([executePlayback]{executePlayback("editor.play");});
        }
        else
        {
            if(playback_.State()==EditorPlaybackState::Playing)
            {
                if(ImGui::Button(Label(text,"EditorUi.Pause").c_str()))Queue([executePlayback]{executePlayback("editor.pause");});
            }
            else if(playback_.State()==EditorPlaybackState::Paused)
            {
                if(ImGui::Button(Label(text,"EditorUi.Resume").c_str()))Queue([executePlayback]{executePlayback("editor.resume");});
                ImGui::SameLine();
                if(ImGui::Button(Label(text,"EditorUi.StepPlay").c_str()))Queue([executePlayback]{executePlayback("editor.step");});
            }
            ImGui::SameLine();
            if(ImGui::Button(Label(text,"EditorUi.Stop").c_str()))Queue([executePlayback]{executePlayback("editor.stop");});
            ImGui::SameLine();
            ImGui::TextUnformatted(text.GetText(editor.PlayPaused()?"EditorUi.Paused":"EditorUi.Running").c_str());
        }
        ImGui::EndDisabled();
        BuildControlToolbar(editor,assets,ui,text);
        if(editor.Dirty()){ImGui::SameLine();ImGui::TextUnformatted(text.GetText("EditorUi.Unsaved").c_str());}
        if(ExportBusy())
        {
            ImGui::SameLine();
            const auto stage=exportSnapshot_.report.stage.empty()?"Editor.ExportStage.Work":exportSnapshot_.report.stage.c_str();
            ImGui::TextUnformatted((std::string(text.GetText("EditorUi.ExportState"))+": "+text.GetText(stage)).c_str());
        }
        if(!errorKey_.empty()){ImGui::SameLine();ImGui::TextUnformatted(text.GetText(errorKey_).c_str());}
        ImGui::End();
    }
    if(!ImGui::GetIO().WantTextInput&&!editor.IsPlaying()&&!gizmo_.Active()&&!inspectorEditing_&&!material_.Editing()&&uiTransaction_.empty()&&!FileDialogActive())
    {
        if(ImGui::GetIO().KeyCtrl&&ImGui::GetIO().KeyShift&&ImGui::GetIO().KeyAlt&&ImGui::IsKeyPressed(ImGuiKey_S,false))Queue([this]{commands_.Execute("editor.save-all",{});});
        else if(ImGui::GetIO().KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_Z,false))Queue([this]{commands_.Execute("editor.undo",{});});
        if(ImGui::GetIO().KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_Y,false))Queue([this]{commands_.Execute("editor.redo",{});});
        else if(ImGui::GetIO().KeyCtrl&&!ImGui::GetIO().KeyShift&&ImGui::IsKeyPressed(ImGuiKey_S,false))Queue([this]{commands_.Execute("editor.save",{});});
    }
    FileDialogs(editor,assets,text);
}

void ImGuiEditor::FileDialogs(EditorSession& editor,AssetStore& assets,const Localization& text)
{
    if(settingsReload_){SettingsReloadDialog(text);return;}
    const auto result=[&](FileActionResult value){if(value==FileActionResult::Failed)errorKey_=files_.LastError();else errorKey_.clear();};
    if(externalDocument_!=ExternalDocument::None)
    {
        const auto title=Label(text,"EditorUi.ExternalChangeTitle");if(!ImGui::IsPopupOpen(title.c_str()))ImGui::OpenPopup(title.c_str());
        if(ImGui::BeginPopupModal(title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(text.GetText("Editor.ExternalChange").c_str());
            ImGui::TextWrapped("%s",asset_io::Utf8(externalDocumentPath_).c_str());
            if(ImGui::Button(Label(text,"EditorUi.ReloadExternal").c_str()))
            {
                bool loaded=false;
                if(externalDocument_==ExternalDocument::ProjectSettings){LoadProjectSettings(true);loaded=projectSettingsError_.empty();}
                else if(externalDocument_==ExternalDocument::BuildSettings){LoadBuildSettings(true);loaded=buildSettingsError_.empty();}
                else loaded=externalDocument_==ExternalDocument::Material?OpenMaterial(assets,externalDocumentPath_,true):(uiOwner_&&OpenUi(*uiOwner_,externalDocumentPath_,true));
                if(loaded){externalDocument_=ExternalDocument::None;externalDocumentPath_.clear();errorKey_.clear();ImGui::CloseCurrentPopup();}
            }
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.SaveAsNew").c_str()))
            {
                auto alternative=externalDocumentPath_;
                const auto extension=alternative.extension().string();
                alternative.replace_filename(alternative.stem().string()+"-external-"+asset_io::NewId().substr(0,8)+extension);
                if(externalDocument_==ExternalDocument::ProjectSettings||externalDocument_==ExternalDocument::BuildSettings)
                {
                    auto& document=externalDocument_==ExternalDocument::ProjectSettings?projectSettingsDocument_:buildSettingsDocument_;
                    RecoveryDocument snapshot;
                    if(document.Snapshot(snapshot,errorKey_)&&asset_io::WriteAtomic(alternative,{reinterpret_cast<const std::uint8_t*>(snapshot.content.data()),snapshot.content.size()},false))
                    {LogConsole(0,"Editor.RecoveryCopySaved",asset_io::Utf8(alternative));externalDocument_=ExternalDocument::None;externalDocumentPath_.clear();ImGui::CloseCurrentPopup();}
                }
                else if(externalDocument_==ExternalDocument::Material)
                {
                    CopyText(materialPath_,asset_io::Utf8(alternative));materialOverwritePath_.clear();materialOverwrite_=false;
                    if(SaveMaterial(assets)){externalDocument_=ExternalDocument::None;externalDocumentPath_.clear();errorKey_.clear();ImGui::CloseCurrentPopup();}
                }
                else if(uiOwner_)
                {
                    if(SaveUi(*uiOwner_,alternative)){CopyText(uiPath_,asset_io::Utf8(alternative));externalDocument_=ExternalDocument::None;externalDocumentPath_.clear();errorKey_.clear();ImGui::CloseCurrentPopup();}
                }
            }
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){externalDocument_=ExternalDocument::None;externalDocumentPath_.clear();errorKey_.clear();ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        return;
    }
    if(files_.TakeExternalSaveAsRequest())saveAsRequested_=true;
    // 保存前发现场景文件被外部修改：先让用户选择，绝不静默覆盖。
    if(files_.NeedsExternalDecision())
    {
        const auto title=Label(text,"EditorUi.ExternalChangeTitle");if(!ImGui::IsPopupOpen(title.c_str()))ImGui::OpenPopup(title.c_str());
        if(ImGui::BeginPopupModal(title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(text.GetText("Editor.ExternalChange").c_str());
            ImGui::TextWrapped("%s",asset_io::Utf8(files_.ExternalTarget()).c_str());
            if(ImGui::Button(Label(text,"EditorUi.ReloadExternal").c_str())){result(files_.DecideExternal(editor,ExternalChoice::Reload));ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.SaveAsNew").c_str())){result(files_.DecideExternal(editor,ExternalChoice::SaveAs));ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){result(files_.DecideExternal(editor,ExternalChoice::Cancel));ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        return;
    }
    // 根作用域一次只绘制一个模态状态，避免从关闭的菜单弹出后失去父作用域。
    if(materialOverwrite_)
    {
        const auto title=Label(text,"EditorUi.OverwriteMaterial");if(!ImGui::IsPopupOpen(title.c_str()))ImGui::OpenPopup(title.c_str());
        if(ImGui::BeginPopupModal(title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(asset_io::Utf8(materialOverwritePath_).c_str());
            if(ImGui::Button(Label(text,"EditorUi.Overwrite").c_str())&&SaveMaterial(assets,true))
            {
                materialOverwrite_=false;ImGui::CloseCurrentPopup();
                if(continueCloseAfterMaterial_){continueCloseAfterMaterial_=false;result(files_.DecideUnsaved(editor,UnsavedChoice::Save,asset_io::FromUtf8(sceneSavePath_.data())));}
            }
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){materialOverwrite_=false;continueCloseAfterMaterial_=false;ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        return;
    }
    if(files_.NeedsOverwriteDecision())
    {
        const auto title=Label(text,"EditorUi.OverwriteScene");if(!ImGui::IsPopupOpen(title.c_str()))ImGui::OpenPopup(title.c_str());
        if(ImGui::BeginPopupModal(title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(asset_io::Utf8(files_.SaveTarget()).c_str());
            if(ImGui::Button(Label(text,"EditorUi.Overwrite").c_str())){result(files_.DecideOverwrite(editor,true));if(!files_.NeedsOverwriteDecision())ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){result(files_.DecideOverwrite(editor,false));ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        return;
    }
    if(files_.NeedsSaveDecision())
    {
        const auto title=Label(text,"EditorUi.UnsavedDocuments");if(!ImGui::IsPopupOpen(title.c_str()))ImGui::OpenPopup(title.c_str());
        if(ImGui::BeginPopupModal(title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize))
        {
            const auto& dirty=documents_.DirtyDocuments();
            if(!externalDocumentsValid_)ImGui::TextWrapped("%s",text.GetText("Editor.DocumentSnapshotFailed").c_str());
            if(!dirty.empty())
            {
                ImGui::TextUnformatted(text.GetText("EditorUi.DirtyDocuments").c_str());
                for(const auto& id:dirty)
                {
                    const auto* key=id=="scene"?"EditorUi.DocumentScene":id=="material"?"EditorUi.DocumentMaterial":id=="ui"?"EditorUi.DocumentUi":id=="script-bindings"?"Editor.DocumentBindings":id=="project-settings"?"EditorUi.ProjectSettings":id=="build-settings"?"EditorUi.BuildSettings":"EditorUi.DocumentUnknown";
                    ImGui::BulletText("%s",text.GetText(key).c_str());
                }
            }
            ImGui::TextUnformatted(text.GetText("EditorUi.DiscardPrompt").c_str());
            if(editor.Path().empty()&&editor.Dirty())ImGui::InputText(Label(text,"EditorUi.SavePath").c_str(),sceneSavePath_.data(),sceneSavePath_.size());
            if(ImGui::Button(Label(text,files_.Closing()?"EditorUi.SaveAndClose":"EditorUi.SaveAndOpen").c_str()))
            {
                const bool ready=!files_.Closing()||SaveAllDocuments(editor,assets,uiOwner_);
                if(ready){result(files_.DecideUnsaved(editor,UnsavedChoice::Save,asset_io::FromUtf8(sceneSavePath_.data())));if(!files_.NeedsSaveDecision())ImGui::CloseCurrentPopup();}
            }
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.DiscardChanges").c_str())){result(files_.DecideUnsaved(editor,UnsavedChoice::Discard,{}));ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){result(files_.DecideUnsaved(editor,UnsavedChoice::Cancel,{}));ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        return;
    }
    if(saveAsRequested_){saveAsRequested_=false;saveAsOpen_=true;ImGui::OpenPopup(Label(text,"EditorUi.SaveSceneAs").c_str());}
    if(saveAsOpen_&&ImGui::BeginPopupModal(Label(text,"EditorUi.SaveSceneAs").c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText(Label(text,"EditorUi.SavePath").c_str(),sceneSavePath_.data(),sceneSavePath_.size());
        if(ImGui::Button(Label(text,"EditorUi.Save").c_str()))
        {const auto value=files_.SaveAs(editor,asset_io::FromUtf8(sceneSavePath_.data()));result(value);if(value!=FileActionResult::Failed){saveAsOpen_=false;ImGui::CloseCurrentPopup();}}
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){saveAsOpen_=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
}

void ImGuiEditor::Hierarchy(EditorSession& editor,const Localization& text)
{
    PreparePanel(EditorPanel::Hierarchy,0,30,260,400);
    if(ImGui::Begin(Label(text,"EditorUi.Hierarchy").c_str(),&showHierarchy_))
    {
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))documentContext_=DocumentContext::Scene;
        ImGui::BeginDisabled(editor.IsPlaying()||gizmo_.Active()||inspectorEditing_);
        if(ImGui::Button(Label(text,"EditorUi.Create").c_str()))Queue([&editor,&text]{editor.Create(text.GetText("EditorUi.NewEntity"));});
        ImGui::SameLine(); ImGui::Button(Label(text,"EditorUi.Root").c_str());
        const auto reparent=[this,&editor](StableEntityId id,StableEntityId parent){
            if(!editor.SetParent(id,parent,keepWorld_?ReparentMode::KeepWorld:ReparentMode::KeepLocal))errorKey_=editor.LastError();else errorKey_.clear();
        };
        StableEntityId dropped=0;if(AcceptEntity(dropped))Queue([reparent,dropped]{reparent(dropped,0);});
        ImGui::Checkbox(Label(text,"EditorUi.KeepWorldTransform").c_str(),&keepWorld_);
        const auto& world=editor.EditWorld();
        std::optional<std::size_t> collapsed;
        for(const auto& item:BuildEditorHierarchy(world))
        {
            if(collapsed&&item.depth>*collapsed)continue;collapsed.reset();
            const auto entity=world.FindByStableId(item.id);
            const auto* record=world.Get(entity);if(!record)continue;
            const auto id=record->stableId;
            ImGui::PushID(std::to_string(id).c_str());
            const float indent=static_cast<float>(item.depth)*12;
            if(indent>0)ImGui::Indent(indent); // Indent(0) 在 ImGui 中表示默认缩进，不是零。
            ImGuiTreeNodeFlags flags=ImGuiTreeNodeFlags_NoTreePushOnOpen|ImGuiTreeNodeFlags_OpenOnArrow|ImGuiTreeNodeFlags_SpanAvailWidth|ImGuiTreeNodeFlags_DefaultOpen;
            if(!item.hasChildren)flags|=ImGuiTreeNodeFlags_Leaf;
            if(editor.Selected()==id)flags|=ImGuiTreeNodeFlags_Selected;
            const bool open=ImGui::TreeNodeEx("entity",flags,"%s",record->name.c_str());
            if(ImGui::IsItemClicked()&&!ImGui::IsItemToggledOpen())Queue([&editor,id]{editor.Select(id);});
            if(!open&&item.hasChildren)collapsed=item.depth;
            if(ImGui::BeginDragDropSource())
            {ImGui::SetDragDropPayload("SE_ENTITY",&id,sizeof(id));ImGui::TextUnformatted(record->name.c_str());ImGui::EndDragDropSource();}
            if(AcceptEntity(dropped))Queue([reparent,dropped,id]{reparent(dropped,id);});
            if(ImGui::BeginPopupContextItem())
            {
                if(ImGui::MenuItem(Label(text,"EditorUi.CreateChild").c_str()))Queue([&editor,id,&text]{editor.Create(text.GetText("EditorUi.NewEntity"),id);});
                if(ImGui::MenuItem(Label(text,"EditorUi.Duplicate").c_str()))Queue([&editor,id]{editor.Duplicate(id);});
                if(ImGui::MenuItem(Label(text,"EditorUi.Delete").c_str()))Queue([&editor,id]{editor.Delete(id);});
                ImGui::EndPopup();
            }
            if(indent>0)ImGui::Unindent(indent);ImGui::PopID();
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void ImGuiEditor::Inspector(EditorSession& editor,AssetStore& assets,const Localization& text)
{
    PreparePanel(EditorPanel::Inspector,1010,30,270,400);
    if(ImGui::Begin(Label(text,"EditorUi.Inspector").c_str(),&showInspector_))
    {
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))documentContext_=DocumentContext::Scene;
        const auto id=editor.Selected();const auto handle=editor.EditWorld().FindByStableId(id);
        const auto* record=editor.EditWorld().Get(handle);
        if(record)
        {
            if(inspected_!=id||inspectedName_!=record->name){inspected_=id;inspectedName_=record->name;CopyText(entityName_,record->name);}
            ImGui::BeginDisabled(editor.IsPlaying()||gizmo_.Active());
            if(ImGui::InputText(Label(text,"EditorUi.Name").c_str(),entityName_.data(),entityName_.size(),ImGuiInputTextFlags_EnterReturnsTrue))
            {const std::string name=entityName_.data();Queue([&editor,id,name]{editor.Rename(id,name);});}
            bool enabled=record->enabled;
            if(ImGui::Checkbox(Label(text,"EditorUi.Enabled").c_str(),&enabled))Queue([&editor,id,enabled]{editor.Apply("Editor.Enabled",[=](World& w){return w.SetEnabled(w.FindByStableId(id),enabled);});});
            for(const auto& component:ComponentMetadata())if(component.key=="transform")ComponentFields(editor,id,component.properties,text);
            const auto* mesh=editor.EditWorld().GetMesh(handle);
            ImGui::TextUnformatted(text.GetText("EditorUi.Mesh").c_str());
            ImGui::Button(mesh?mesh->meshAsset.c_str():Label(text,"EditorUi.DropAsset").c_str(),{-1,32});
            AssetId asset;if(AcceptAsset(asset))
            {
                std::vector<AssetDiagnostic> diagnostics;
                if(assets.LoadModel(asset,diagnostics))Queue([&editor,id,asset]{editor.SetMesh(id,MeshComponent{asset,{}});});
                else errorKey_="Assets.UnsupportedFormat";
            }
            if(mesh)
            {
                std::size_t slots=1;
                if(!IsBuiltinMesh(mesh->meshAsset)){std::vector<AssetDiagnostic> diagnostics;if(const auto model=assets.LoadModel(mesh->meshAsset,diagnostics))slots=std::max(size_t{1},model->materials.size());}
                ImGui::InputInt(Label(text,"EditorUi.MaterialSlot").c_str(),&materialSlot_);materialSlot_=std::clamp(materialSlot_,0,static_cast<int>(std::min(size_t{1024},slots)-1));
                const auto slot=static_cast<std::size_t>(materialSlot_);
                const auto current=slot<mesh->materials.size()?mesh->materials[slot]:std::string{};
                ImGui::Button(current.empty()?Label(text,"EditorUi.DropMaterial").c_str():current.c_str(),{-1,28});
                if(AcceptAsset(asset))
                {
                    std::vector<AssetDiagnostic> diagnostics;
                    if(assets.LoadMaterial(asset,diagnostics))
                    {auto changed=*mesh;changed.materials.resize(std::max(changed.materials.size(),slot+1));changed.materials[slot]=asset;Queue([&editor,id,changed]{editor.SetMesh(id,changed);});}
                    else errorKey_="Assets.InvalidMaterial";
                }
                if(ImGui::Button(Label(text,"EditorUi.ClearMaterial").c_str()))
                {auto changed=*mesh;if(slot<changed.materials.size())changed.materials[slot].clear();Queue([&editor,id,changed]{editor.SetMesh(id,changed);});}
            }
            ImGui::BeginDisabled(HasComponent(editor.EditWorld(),handle,"camera"));
            if(ImGui::Button(Label(text,"EditorUi.AddCamera").c_str()))Queue([&editor,id]{editor.Apply("Editor.Camera",[=](World&w){std::string error;return AddComponent(w,w.FindByStableId(id),"camera",error);});});
            ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(HasComponent(editor.EditWorld(),handle,"light"));
            if(ImGui::Button(Label(text,"EditorUi.AddLight").c_str()))Queue([&editor,id]{editor.Apply("Editor.Light",[=](World&w){std::string error;return AddComponent(w,w.FindByStableId(id),"light",error);});});
            ImGui::EndDisabled();
            for(const auto& component:ComponentMetadata())
                if(component.key!="transform"&&HasComponent(editor.EditWorld(),handle,component.key))ComponentFields(editor,id,component.properties,text);
            if(scriptInspector_)scriptInspector_(id,editor.IsPlaying());
            ImGui::EndDisabled();
        }
        else ImGui::TextUnformatted(text.GetText("EditorUi.NothingSelected").c_str());
        TODInspector(editor,assets,text);
    }
    ImGui::End();
}

void ImGuiEditor::TODInspector(EditorSession& editor,AssetStore& assets,const Localization& text)
{
    if(!ImGui::CollapsingHeader(Label(text,"TOD.Editor").c_str()))return;
    auto settings=editor.EditWorld().EnvironmentSettings();
    ImGui::BeginDisabled(editor.IsPlaying());
    ImGui::Button(settings.todProfile.empty()?Label(text,"TOD.DropProfile").c_str():settings.todProfile.c_str(),{-1,26});
    AssetId id;if(AcceptAsset(id))
    {
        std::vector<AssetDiagnostic> diagnostics;const auto profile=assets.LoadTODProfile(id,diagnostics);std::string error;
        if(profile&&TODSystem::ValidateProfile(*profile,1.0/60,error)){settings.todProfile=id;Queue([&editor,settings]{editor.Apply("TOD.Editor",[&](World& world){return world.SetEnvironmentSettings(settings);});});}
        else if(!error.empty())errorKey_=error;
        else errorKey_=diagnostics.empty()?"TOD.ReadFailed":diagnostics.back().key;
    }
    if(ImGui::Checkbox(Label(text,"TOD.PreserveTime").c_str(),&settings.preserveTime))Queue([&editor,settings]{editor.Apply("TOD.Editor",[&](World& world){return world.SetEnvironmentSettings(settings);});});
    if(ImGui::Button(Label(text,"TOD.ClearProfile").c_str()))Queue([&editor]{editor.Apply("TOD.Editor",[](World& world){return world.SetEnvironmentSettings({});});});
    ImGui::EndDisabled();
    if(!settings.todProfile.empty()&&ImGui::Button(Label(text,todPreview_.Dirty()?"TOD.ReloadDiscard":"TOD.OpenPreview").c_str()))
        if(!todPreview_.Open(assets,settings.todProfile))errorKey_=todPreview_.LastError();
    if(!todPreview_.Ready())return;
    auto candidate=todPreview_.Profile();bool changed=false;
    changed|=ImGui::InputDouble(Label(text,"TOD.DayLength").c_str(),&candidate.dayLengthSeconds);
    changed|=ImGui::InputDouble(Label(text,"TOD.InitialTime").c_str(),&candidate.initialTime);
    changed|=ImGui::InputDouble(Label(text,"TOD.Rate").c_str(),&candidate.rate);
    changed|=ImGui::InputDouble(Label(text,"TOD.Sunrise").c_str(),&candidate.sunrise);
    changed|=ImGui::InputDouble(Label(text,"TOD.Sunset").c_str(),&candidate.sunset);
    changed|=ImGui::InputFloat(Label(text,"TOD.Azimuth").c_str(),&candidate.azimuthRadians);
    changed|=ImGui::InputFloat(Label(text,"TOD.Elevation").c_str(),&candidate.maximumElevationRadians);
    for(std::size_t i=0;i<candidate.keys.size();++i)
    {
        ImGui::PushID(static_cast<int>(i));auto& key=candidate.keys[i];
        if(ImGui::TreeNode(Label(text,"TOD.CurveKey").c_str()))
        {
            changed|=ImGui::InputDouble(Label(text,"TOD.Time").c_str(),&key.time);
            changed|=ImGui::ColorEdit3(Label(text,"TOD.SunColor").c_str(),&key.value.sunColor.x,ImGuiColorEditFlags_Float|ImGuiColorEditFlags_HDR);
            changed|=ImGui::InputFloat(Label(text,"TOD.Intensity").c_str(),&key.value.sunIntensity);
            changed|=ImGui::ColorEdit3(Label(text,"TOD.SkyColor").c_str(),&key.value.skyColor.x,ImGuiColorEditFlags_Float|ImGuiColorEditFlags_HDR);
            changed|=ImGui::ColorEdit3(Label(text,"TOD.FogColor").c_str(),&key.value.fogColor.x,ImGuiColorEditFlags_Float|ImGuiColorEditFlags_HDR);
            changed|=ImGui::InputFloat(Label(text,"TOD.FogDensity").c_str(),&key.value.fogDensity);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if(changed&&!todPreview_.Apply(candidate))errorKey_=todPreview_.LastError();
    bool paused=todPreview_.Paused();if(ImGui::Checkbox(Label(text,"TOD.PreviewPaused").c_str(),&paused))todPreview_.SetPaused(paused);
    float time=static_cast<float>(todPreview_.Snapshot().time);
    if(ImGui::SliderFloat(Label(text,"TOD.PreviewTime").c_str(),&time,0,0.999999f))todPreview_.SetTime(time);
    const auto snapshot=todPreview_.Snapshot();ImGui::Text("%s: %.6f",text.GetText("TOD.Time").c_str(),snapshot.time);
    ImGui::ColorButton(Label(text,"TOD.SkyColor").c_str(),{snapshot.parameters.skyColor.x,snapshot.parameters.skyColor.y,snapshot.parameters.skyColor.z,1});
    ImGui::SameLine();ImGui::TextUnformatted(text.GetText("TOD.CpuPreview").c_str());
    if(todPreview_.Dirty())ImGui::TextUnformatted(text.GetText("TOD.Unsaved").c_str());
    if(ImGui::Button(Label(text,"TOD.SaveProfile").c_str())&&!todPreview_.Save())errorKey_=todPreview_.LastError();
}

void ImGuiEditor::ComponentFields(EditorSession& editor,StableEntityId id,std::span<const PropertyDescriptor> fields,const Localization& text)
{
    for(const auto& field:fields)
    {
        PropertyValue value;std::string error;
        if(!ReadProperty(editor.EditWorld(),editor.EditWorld().FindByStableId(id),field.key,value,error)){errorKey_=error;continue;}
        const auto result=DrawPropertyWidget(field,value,text);if(!result.supported)continue;
        if(result.activated)Queue([this,&editor,key=std::string(field.key),label=std::string(field.labelKey)]{
            if(inspectorEditing_&&inspectorField_!=key){editor.CommitEdit();inspectorEditing_=false;}
            if(!inspectorEditing_){inspectorEditing_=editor.BeginEdit(label);if(inspectorEditing_)inspectorField_=key;}
        });
        if(result.changed)Queue([this,&editor,id,key=std::string(field.key),value]{if(!editor.SetProperty(id,key,value))errorKey_=editor.LastError();else errorKey_.clear();});
        if(result.deactivated)Queue([this,&editor,key=std::string(field.key)]{if(inspectorEditing_&&inspectorField_==key){editor.CommitEdit();inspectorEditing_=false;inspectorField_.clear();}});
    }
}

void ImGuiEditor::PreviewAsset(AssetStore& assets,const AssetId& id)
{
    selectedAsset_=id;previewModel_.reset();previewImage_.reset();assetDiagnostics_.clear();const auto path=assets.Resolve(id);
    previewPending_.clear();
    if(IsMaterialDocumentPath(path)){showMaterial_=true;OpenMaterial(assets,path);return;}
    previewPending_=id;
}
void ImGuiEditor::PollAssetPreview(AssetStore& assets)
{
    using namespace std::chrono_literals;
    if(previewTask_.valid()&&previewTask_.wait_for(0s)==std::future_status::ready)
    {
        try{auto result=previewTask_.get();if(result.id==selectedAsset_){previewModel_=std::move(result.model);previewImage_=std::move(result.image);assetDiagnostics_=std::move(result.diagnostics);}}
        catch(const std::exception& error){LogConsole(2,"Editor.AssetPreviewFailed",error.what());}
    }
    if(!previewTask_.valid()&&!previewPending_.empty())
    {
        const auto id=std::exchange(previewPending_,{});AssetSourceSnapshot source;
        if(!assets.DescribeSource(id,source,assetDiagnostics_))return;
        previewTask_=std::async(std::launch::async,[source]{
            PreviewResult result;result.id=source.id;auto extension=asset_io::Utf8(source.source.extension());
            std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});
            if(extension==".fbx"||extension==".gltf"||extension==".glb")result.model=AssetStore::LoadModelSnapshot(source,result.diagnostics);
            else if(extension!=".sebin"&&extension!=".setod")result.image=AssetStore::LoadImageSnapshot(source,result.diagnostics);
            return result;
        });
    }
}
void ImGuiEditor::Assets(EditorSession& editor,AssetStore& assets,const Localization& text)
{
    PreparePanel(EditorPanel::Assets,0,440,760,280);
    if(ImGui::Begin(Label(text,"EditorUi.Assets").c_str(),&showAssets_))
    {
        ImGui::BeginDisabled(editor.IsPlaying()||gizmo_.Active()||inspectorEditing_||FileDialogActive());
        ImGui::SetNextItemWidth(420);ImGui::InputText(Label(text,"EditorUi.SourcePath").c_str(),importPath_.data(),importPath_.size());
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Import").c_str()))
        {
            QueueAssetImport({asset_io::FromUtf8(importPath_.data())});
        }
        ImGui::SameLine();if(ImGui::Button(Label(text,"Editor.AssetChooseFiles").c_str()))
        {
            std::vector<wchar_t> buffer(65536,0);OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);
            dialog.hwndOwner=static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
            dialog.lpstrFile=buffer.data();dialog.nMaxFile=static_cast<DWORD>(buffer.size());
            dialog.lpstrFilter=L"Assets\0*.gltf;*.glb;*.fbx;*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds;*.sebin;*.json\0*.*\0*.*\0";
            dialog.Flags=OFN_ALLOWMULTISELECT|OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(GetOpenFileNameW(&dialog))
            {
                const std::filesystem::path first(buffer.data());const auto* next=buffer.data()+wcslen(buffer.data())+1;
                std::vector<std::filesystem::path> sources;
                if(!*next)sources.push_back(first);
                else for(;*next;next+=wcslen(next)+1)sources.push_back(first/next);
                QueueAssetImport(std::move(sources));
            }
            else if(CommDlgExtendedError())errorKey_="Editor.AssetChooseFailed";
        }
        if(!importReport_.empty())ImGui::TextWrapped("%s",importReport_.c_str());
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Rescan").c_str())){assetDiagnostics_.clear();if(assets.Scan(assetDiagnostics_))SyncAssetDeclarations(assets);}
        ImGui::SameLine();if(ImGui::Checkbox(Label(text,"Editor.BrowseProjectFiles").c_str(),&browseProjectFiles_))assetFolder_.clear();
        AssetActionsToolbar(editor,assets,text);
        if(browseProjectFiles_)ProjectFiles(editor,assets,text);
        else
        {
        ImGui::SetNextItemWidth(180);ImGui::InputText(Label(text,"Editor.AssetSearch").c_str(),assetSearch_.data(),assetSearch_.size());
        ImGui::SameLine();ImGui::SetNextItemWidth(140);
        const char* kinds[]={"Editor.AssetAllTypes","Editor.AssetModels","Editor.AssetImages","Editor.AssetMaterials","Editor.AssetShaders"};
        if(ImGui::BeginCombo(Label(text,"Editor.AssetType").c_str(),text.GetText(kinds[assetType_]).c_str()))
        {for(int i=0;i<5;++i)if(ImGui::Selectable(text.GetText(kinds[i]).c_str(),assetType_==i))assetType_=i;ImGui::EndCombo();}
        ImGui::TextWrapped("%s",assetFolder_.empty()?text.GetText("Editor.AssetRoot").c_str():asset_io::Utf8(assetFolder_).c_str());
        if(!assetFolder_.empty()&&ImGui::Button(Label(text,"Editor.AssetParent").c_str()))assetFolder_=assetFolder_.parent_path();
        std::vector<std::filesystem::path> folders;
        std::error_code folderError;
        for(std::filesystem::directory_iterator it(assets.SourceRoot()/assetFolder_,folderError),end;!folderError&&it!=end;it.increment(folderError))
            if(it->is_directory(folderError)&&asset_io::Within(assets.SourceRoot(),it->path()))folders.push_back(it->path().filename());
        std::sort(folders.begin(),folders.end());
        for(const auto& folder:folders)
        {const auto name=asset_io::Utf8(folder);if(ImGui::Selectable(("[+] "+name).c_str(),false,ImGuiSelectableFlags_AllowDoubleClick)&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))assetFolder_/=folder;}
        const auto lower=[](std::string value){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});return value;};
        for(const auto& id:assets.ListIds())
        {
            const auto path=assets.Resolve(id);const auto filename=asset_io::Utf8(path.filename());
            const auto relative=path.lexically_relative(assets.SourceRoot());
            if(relative.parent_path()!=assetFolder_)continue;
            const auto extension=lower(asset_io::Utf8(path.extension()));
            const int kind=IsMaterialDocumentPath(path)?3:(extension==".gltf"||extension==".glb"||extension==".fbx")?1:extension==".sebin"?4:2;
            if((assetType_&&assetType_!=kind)||lower(filename).find(lower(assetSearch_.data()))==std::string::npos)continue;
            ImGui::PushID(id.c_str());
            if(ImGui::Selectable(("["+text.GetText(kinds[kind])+"] "+filename).c_str(),selectedAsset_==id,ImGuiSelectableFlags_AllowDoubleClick))
            {
                selectedAsset_=id;previewModel_.reset();previewImage_.reset();
                CopyText(assetTarget_,asset_io::Utf8(relative));
                if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    PreviewAsset(assets,id);
                }
            }
            if(ImGui::BeginDragDropSource())
            {ImGui::SetDragDropPayload("SE_ASSET",id.data(),id.size());ImGui::TextUnformatted(filename.c_str());ImGui::EndDragDropSource();}
            ImGui::PopID();
        }
        }
        if(previewModel_)ImGui::Text("%s: %zu / %s: %zu",text.GetText("EditorUi.MeshCount").c_str(),previewModel_->meshes.size(),text.GetText("EditorUi.NodeCount").c_str(),previewModel_->nodes.size());
        if(previewTask_.valid()||!previewPending_.empty())ImGui::TextUnformatted(text.GetText("Editor.AssetPreviewLoading").c_str());
        if(previewImage_)ImGui::Text("%s: %u x %u",text.GetText("EditorUi.ImageSize").c_str(),previewImage_->width,previewImage_->height);
        if(previewModel_)
        {
            if(ImGui::Button(Label(text,"EditorUi.AddToScene").c_str()))Queue([this,&editor,name=asset_io::Utf8(assets.Resolve(selectedAsset_).stem()),asset=selectedAsset_]{
                if(editor.BeginEdit("Editor.Mesh")){const auto id=editor.Create(name);if(id&&editor.SetMesh(id,{asset,{}}))editor.CommitEdit();else{errorKey_=editor.LastError();editor.CancelEdit();}}
            });
            for(size_t index=0;index<previewModel_->materials.size();++index)
            {const auto& material=previewModel_->materials[index];ImGui::Text("%s %zu: %s%s",text.GetText("EditorUi.MaterialSlot").c_str(),index,material.name.c_str(),material.needsFallbackTexture?text.GetText("EditorUi.FallbackTexture").c_str():"");}
        }
        for(const auto& diagnostic:assetDiagnostics_)
            ImGui::TextWrapped("%s: %s",text.GetText(diagnostic.key).c_str(),diagnostic.detail.c_str());
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void ImGuiEditor::QueueAssetImport(std::vector<std::filesystem::path> sources)
{
    if(sources.empty())return;
    if(AssetOperationBusy()||importCandidate_||assetActionCandidate_){errorKey_="Editor.AssetOperationBusy";return;}
    importSources_=std::move(sources);showAssets_=true;
}
void ImGuiEditor::ImportDialogs(EditorSession& editor,AssetStore& assets,const Localization& text)
{
    using namespace std::chrono_literals;
    PollAssetPreview(assets);
    if(!importSources_.empty())
    {
        if(editor.IsPlaying()){errorKey_="Editor.EditWhilePlaying";importSources_.clear();}
        else
        {
            AssetImportRequest request{std::move(importSources_),assets.SourceRoot(),browseProjectFiles_?std::filesystem::path{}:assetFolder_};importSources_.clear();
            importPlanning_=std::async(std::launch::async,[request]{return AssetImportService::Plan(request);});
        }
    }
    if(importPlanning_.valid()&&importPlanning_.wait_for(0s)==std::future_status::ready)
    {
        try{importCandidate_=importPlanning_.get();}
        catch(const std::exception& exception){errorKey_="Editor.AssetImportFailed";LogConsole(2,errorKey_,exception.what());}
    }
    if(importExecution_.valid()&&importExecution_.wait_for(0s)==std::future_status::ready)
    {
        try
        {
            const auto result=importExecution_.get();assetDiagnostics_.clear();
            if(!assets.Scan(assetDiagnostics_))errorKey_="Assets.ScanFailed";
            else if(!result.success){errorKey_=result.key;LogConsole(2,result.key,result.detail);}
            else
            {
                errorKey_.clear();LogConsole(0,"Editor.AssetImportSucceeded",std::to_string(result.imported.size()));
                std::vector<EditorAssetIssue> issues;
                if(!EditorAssetActions::SyncProjectAssets(projectRoot_,assets,issues))
                    for(const auto& issue:issues)LogConsole(2,issue.key,issue.detail);
                if(!result.imported.empty())selectedAsset_=result.imported.front();
            }
        }
        catch(const std::exception& exception){errorKey_="Editor.AssetImportFailed";LogConsole(2,errorKey_,exception.what());}
    }
    if(AssetOperationBusy())
    {ImGui::OpenPopup("asset-operation");if(ImGui::BeginPopupModal("asset-operation",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::TextUnformatted(text.GetText("Editor.AssetOperationBusy").c_str());ImGui::EndPopup();}}
    else if(ImGui::BeginPopupModal("asset-operation",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::CloseCurrentPopup();ImGui::EndPopup();}
    if(importCandidate_)ImGui::OpenPopup("asset-import-confirm");
    if(ImGui::BeginPopupModal("asset-import-confirm",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        if(importCandidate_)
        {
            const auto& plan=*importCandidate_;
            ImGui::TextUnformatted(text.GetText("Editor.AssetImportConfirm").c_str());
            ImGui::BeginChild("import-files",ImVec2(680,280),ImGuiChildFlags_Borders);
            for(const auto& file:plan.files)
                ImGui::TextWrapped("%s: %s -> %s",text.GetText(file.primary?"Editor.AssetPrimary":"Editor.AssetDependency").c_str(),asset_io::Utf8(file.source).c_str(),asset_io::Utf8(file.target).c_str());
            for(const auto& diagnostic:plan.diagnostics)ImGui::TextWrapped("%s: %s",text.GetText(diagnostic.key).c_str(),diagnostic.detail.c_str());
            if(!plan.errorKey.empty())ImGui::TextWrapped("%s: %s",text.GetText(plan.errorKey).c_str(),plan.detail.c_str());
            ImGui::EndChild();ImGui::BeginDisabled(!plan.valid);
            if(ImGui::Button(Label(text,"EditorUi.Import").c_str()))
            {
                const auto candidate=plan;const auto cache=projectRoot_/"out"/"Cache"/"Assets";
                importExecution_=std::async(std::launch::async,[candidate,cache]{
                    ImportResult result;AssetStore worker(candidate.contentRoot,cache);
                    std::vector<AssetDiagnostic> diagnostics;
                    if(!worker.Scan(diagnostics)){result.key=diagnostics.empty()?"Assets.ScanFailed":diagnostics.back().key;return result;}
                    result.success=AssetImportService::ExecuteRegistered(candidate,worker,result.imported,result.key,result.detail);return result;
                });
                importCandidate_.reset();ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();ImGui::SameLine();
            if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){importCandidate_.reset();ImGui::CloseCurrentPopup();}
        }
        ImGui::EndPopup();
    }
    AssetActionDialogs(editor,assets,text);
}

bool ImGuiEditor::MaterialDirty() const
{
    return materialReady_&&material_.Dirty();
}
std::optional<MaterialPreview> ImGuiEditor::CurrentMaterialPreview() const
{
    if(!materialReady_||activeMaterialId_.empty()||!material_.Dirty())return {};
    return MaterialPreview{activeMaterialId_,material_.Document()};
}
bool ImGuiEditor::OpenMaterialForToolkit(AssetStore& assets,const std::filesystem::path& path,bool discard,
    std::string& errorKey)
{
    const bool opened=OpenMaterial(assets,path,discard);
    errorKey=opened?std::string{}:errorKey_;
    return opened;
}
bool ImGuiEditor::SaveMaterialForToolkit(AssetStore& assets,std::string& errorKey)
{
    const bool saved=SaveMaterial(assets);
    errorKey=saved?std::string{}:errorKey_;
    return saved;
}
bool ImGuiEditor::SetMaterialPropertyForToolkit(std::string_view key,const PropertyValue& value)
{
    if(!materialReady_)return false;
    if(!material_.Editing()&&!material_.BeginEdit())return false;
    if(!material_.SetProperty(key,value)){material_.CancelEdit();return false;}
    return material_.CommitEdit();
}
bool ImGuiEditor::BeginUiCanvasDragForToolkit(UiTree& ui,UiId id,UiPoint screen)
{
    BeginUiEdit(ui);
    if(!uiCanvas_.Begin(ui,id,UiCanvasDrag::Move,screen))
    {
        EndUiEdit(ui,false);
        return false;
    }
    selectedUi_=id;
    return true;
}
bool ImGuiEditor::UpdateUiCanvasDragForToolkit(UiTree& ui,UiPoint screen)
{
    return uiCanvas_.Update(ui,screen);
}
bool ImGuiEditor::EndUiCanvasDragForToolkit(UiTree& ui,bool commit)
{
    const bool finished=uiCanvas_.End(ui,commit);
    EndUiEdit(ui,commit&&finished);
    return finished;
}
bool ImGuiEditor::SaveMaterial(AssetStore& assets,bool overwrite)
{
    if(!materialReady_)return false;
    std::error_code pathError;const auto requested=asset_io::FromUtf8(materialPath_.data());
    const auto target=overwrite?materialOverwritePath_:std::filesystem::weakly_canonical(requested.is_absolute()?requested:assets.SourceRoot()/requested,pathError);
    if(pathError||target.empty()||!IsMaterialDocumentPath(target)){errorKey_="Assets.MaterialPathInvalid";return false;}
    if(!asset_io::Within(assets.SourceRoot(),target)){errorKey_="Assets.ReferenceOutsideRoot";return false;}
    const bool sameActive=!activeMaterialPath_.empty()&&asset_io::Within(target,activeMaterialPath_)&&asset_io::Within(activeMaterialPath_,target);
    if(!overwrite&&sameActive&&!CheckExternalDocument(ExternalDocument::Material,target))return false;
    if(!overwrite&&std::filesystem::exists(target)&&!sameActive)
    {materialOverwritePath_=target;materialOverwrite_=true;return false;}
    if(overwrite&&!std::filesystem::exists(std::filesystem::path(target.wstring()+L".asset.json")))
    {AssetId imported;assetDiagnostics_.clear();if(!assets.RegisterSource(target,{},imported,assetDiagnostics_)){errorKey_=assetDiagnostics_.empty()?"Assets.InvalidMetadata":assetDiagnostics_.back().key;return false;}}
    if(material_.Editing()){material_.CommitEdit();materialField_.clear();}
    AssetId id;assetDiagnostics_.clear();
    if(!assets.SaveMaterialSource(target,material_.Document(),id,assetDiagnostics_))
    {errorKey_=assetDiagnostics_.empty()?"Assets.InvalidMaterial":assetDiagnostics_.back().key;return false;}
    if(!material_.MarkSaved()){errorKey_=material_.LastError();return false;}
    activeMaterialPath_=target;CopyText(materialPath_,asset_io::Utf8(target));activeMaterialId_=id;selectedAsset_=id;RecordStamp(materialStamp_,target);errorKey_.clear();SyncAssetDeclarations(assets);return true;
}
bool ImGuiEditor::OpenMaterial(AssetStore& assets,const std::filesystem::path& requested,bool discard)
{
    if(!discard&&MaterialDirty()){errorKey_="Editor.UnsavedChanges";return false;}
    const auto path=std::filesystem::weakly_canonical(requested.is_absolute()?requested:assets.SourceRoot()/requested);
    if(!asset_io::Within(assets.SourceRoot(),path)){errorKey_="Assets.ReferenceOutsideRoot";return false;}
    MaterialDocument loaded;assetDiagnostics_.clear();
    if(!LoadMaterialDocument(path,loaded,assetDiagnostics_)){errorKey_=assetDiagnostics_.empty()?"Assets.InvalidMaterial":assetDiagnostics_.back().key;return false;}
    AssetId id;
    for(const auto& candidate:assets.ListIds())if(asset_io::Within(path,assets.Resolve(candidate))&&asset_io::Within(assets.Resolve(candidate),path)){id=candidate;break;}
    const bool newlyRegistered=id.empty();
    if(newlyRegistered&&!assets.RegisterSource(path,{},id,assetDiagnostics_)){errorKey_=assetDiagnostics_.empty()?"Assets.InvalidMetadata":assetDiagnostics_.back().key;return false;}
    if(!material_.Reset(loaded,true)){errorKey_=material_.LastError();return false;}
    activeMaterialPath_=path;activeMaterialId_=id;materialField_.clear();documentContext_=DocumentContext::Material;materialReady_=true;CopyText(materialPath_,asset_io::Utf8(path));RecordStamp(materialStamp_,path);if(newlyRegistered)SyncAssetDeclarations(assets);return true;
}
void ImGuiEditor::MaterialEditor(EditorSession& editor,AssetStore& assets,const Localization& text)
{
    PreparePanel(EditorPanel::MaterialEditor,320,180,520,440);
    if(ImGui::Begin(Label(text,"EditorUi.MaterialEditor").c_str()))
    {
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))documentContext_=DocumentContext::Material;
        const auto newMaterial=[&]{MaterialDocument document;document.name=text.GetText("EditorUi.NewMaterial");material_.Reset(document,false);activeMaterialId_.clear();activeMaterialPath_.clear();CopyText(materialPath_,"Materials/NewMaterial-"+asset_io::NewId()+".material.json");materialField_.clear();documentContext_=DocumentContext::Material;materialReady_=true;};
        ImGui::InputText(Label(text,"EditorUi.MaterialPath").c_str(),materialPath_.data(),materialPath_.size());
        if(ImGui::Button(Label(text,"EditorUi.NewMaterial").c_str()))
        {
            if(MaterialDirty())ImGui::OpenPopup("new-material");
            else newMaterial();
        }
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.OpenMaterial").c_str()))
        {if(!OpenMaterial(assets,asset_io::FromUtf8(materialPath_.data()))&&errorKey_=="Editor.UnsavedChanges")ImGui::OpenPopup("open-material");}
        ImGui::SameLine();ImGui::BeginDisabled(!materialReady_);
        if(ImGui::Button(Label(text,"EditorUi.SaveMaterial").c_str()))
            SaveMaterial(assets);
        ImGui::EndDisabled();
        if(ImGui::BeginPopup("new-material"))
        {
            ImGui::TextUnformatted(text.GetText("EditorUi.DiscardPrompt").c_str());
            if(ImGui::Button(Label(text,"EditorUi.DiscardNew").c_str()))
            {newMaterial();ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str()))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        if(ImGui::BeginPopup("open-material"))
        {
            ImGui::TextUnformatted(text.GetText("EditorUi.DiscardPrompt").c_str());
            if(ImGui::Button(Label(text,"EditorUi.DiscardOpen").c_str())){OpenMaterial(assets,asset_io::FromUtf8(materialPath_.data()),true);ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str()))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        if(materialReady_)
        {
            if(!activeMaterialPath_.empty())ImGui::TextWrapped("%s: %s",text.GetText("EditorUi.CurrentDocument").c_str(),asset_io::Utf8(activeMaterialPath_.filename()).c_str());
            const auto selected=editor.Selected();const auto* selectedMesh=editor.EditWorld().GetMesh(editor.EditWorld().FindByStableId(selected));
            ImGui::BeginDisabled(!selectedMesh||activeMaterialId_.empty());
            if(ImGui::Button(Label(text,"EditorUi.AssignMaterial").c_str()))
            {
                auto materials=selectedMesh->materials;const auto slot=static_cast<size_t>(std::clamp(materialSlot_,0,1023));materials.resize(std::max(materials.size(),slot+1));materials[slot]=activeMaterialId_;
                Queue([this,&editor,selected,materials]{if(!editor.SetProperty(selected,"mesh.materials",materials))errorKey_=editor.LastError();});
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(material_.Editing()||material_.UndoCount()==0);
            if(ImGui::Button(Label(text,"EditorUi.Undo").c_str()))material_.Undo();ImGui::EndDisabled();ImGui::SameLine();
            ImGui::BeginDisabled(material_.Editing()||material_.RedoCount()==0);
            if(ImGui::Button(Label(text,"EditorUi.Redo").c_str()))material_.Redo();ImGui::EndDisabled();
            if(material_.Editing()&&ImGui::IsKeyPressed(ImGuiKey_Escape,false)){material_.CancelEdit();materialField_.clear();ImGui::ClearActiveID();}
            for(const auto& field:MaterialProperties())
            {
                if(field.type==PropertyType::AssetId)continue;
                PropertyValue value;std::string error;
                if(!ReadMaterialProperty(material_.Document(),field.key,value,error)){errorKey_=error;continue;}
                const auto result=DrawPropertyWidget(field,value,text);
                if(result.activated)
                {
                    if(material_.Editing()&&materialField_!=field.key)material_.CommitEdit();
                    if(!material_.Editing()){material_.BeginEdit();materialField_=field.key;}
                }
                if(result.changed){if(!material_.SetProperty(field.key,value))errorKey_=material_.LastError();else errorKey_.clear();}
                if(result.deactivated&&material_.Editing()&&materialField_==field.key){material_.CommitEdit();materialField_.clear();}
            }
            const auto textureRow=[&](const char* property)
            {
                const auto* field=FindMaterialProperty(property);PropertyValue value;std::string error;
                if(!field||!ReadMaterialProperty(material_.Document(),property,value,error))return;
                const auto& texture=std::get<std::string>(value);ImGui::PushID(property);ImGui::TextUnformatted(text.GetText(std::string(field->labelKey)).c_str());
                ImGui::Button(texture.empty()?Label(text,"EditorUi.DropImage").c_str():texture.c_str(),{330,25});
                AssetId asset;if(AcceptAsset(asset))
                {assetDiagnostics_.clear();if(assets.LoadTextureImage(asset,assetDiagnostics_)){if(!material_.SetProperty(property,asset))errorKey_=material_.LastError();}else errorKey_="Assets.ImageDecodeFailed";}
                ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.ClearTexture").c_str()))material_.SetProperty(property,std::string{});ImGui::PopID();
            };
            textureRow("material.baseColorTexture");textureRow("material.normalTexture");textureRow("material.metallicRoughnessTexture");
            const auto shaderRow=[&]
            {
                const auto* field=FindMaterialProperty("material.shader");PropertyValue value;std::string error;
                if(!field||!ReadMaterialProperty(material_.Document(),"material.shader",value,error))return;
                const auto& shader=std::get<std::string>(value);ImGui::PushID("material.shader");
                ImGui::TextUnformatted(text.GetText(std::string(field->labelKey)).c_str());
                ImGui::Button(Label(text,"EditorUi.DropShader").c_str(),{330,25});
                if(!shader.empty())
                {
                    ImGui::SameLine();
                    const auto filename=asset_io::Utf8(assets.Resolve(shader).filename());
                    ImGui::TextUnformatted(filename.empty()?shader.c_str():filename.c_str());
                }
                AssetId asset;if(AcceptAsset(asset))
                {
                    assetDiagnostics_.clear();ShaderArtifactSource artifact;
                    if(assets.LoadShaderArtifact(asset,artifact,assetDiagnostics_))
                    {if(!material_.SetProperty("material.shader",asset))errorKey_=material_.LastError();else errorKey_.clear();}
                    else errorKey_=assetDiagnostics_.empty()?"Assets.ShaderArtifactInvalid":assetDiagnostics_.back().key;
                }
                ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.ClearShader").c_str()))material_.SetProperty("material.shader",std::string{});
                ImGui::PopID();
            };
            shaderRow();
            if(MaterialDirty())ImGui::TextUnformatted(text.GetText("EditorUi.Unsaved").c_str());
        }
    }
    ImGui::End();
}

EditorRect ImGuiEditor::Viewport(EditorSession& editor,AssetStore& assets,const Localization& text,const EditorViewport& requestedViewport)
{
    auto viewport=requestedViewport;
    PreparePanel(EditorPanel::Viewport,270,30,730,380);
    EditorRect rect;
    if(ImGui::Begin(Label(text,"EditorUi.Viewport").c_str(),&showViewport_))
    {
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))documentContext_=DocumentContext::Scene;
        for(const auto& item:{std::pair{GizmoMode::Translate,"EditorUi.Translate"},std::pair{GizmoMode::Rotate,"EditorUi.Rotate"},std::pair{GizmoMode::Scale,"EditorUi.ScaleTool"}})
        {if(item.first!=GizmoMode::Translate)ImGui::SameLine();if(ImGui::RadioButton(Label(text,item.second).c_str(),mode_==item.first))mode_=item.first;}
        ImGui::SameLine();if(ImGui::RadioButton(Label(text,"EditorUi.WorldSpace").c_str(),space_==GizmoSpace::World))space_=GizmoSpace::World;
        ImGui::SameLine();if(ImGui::RadioButton(Label(text,"EditorUi.LocalSpace").c_str(),space_==GizmoSpace::Local))space_=GizmoSpace::Local;
        const auto origin=ImGui::GetCursorScreenPos(),available=ImGui::GetContentRegionAvail();
        rect={origin.x,origin.y,std::max(1.0f,available.x),std::max(1.0f,available.y)};
        if(viewport.prepare)viewport.prepare(rect,!editor.IsPlaying()&&!gizmo_.Active()&&!inspectorEditing_&&!material_.Editing()&&!FileDialogActive(),viewport);
        auto dragCamera=viewport.camera;if(dragCamera)dragCamera->viewport={rect.x,rect.y,rect.width,rect.height};
        if(gizmo_.Active()&&!gizmo_.ProjectionMatches(dragCamera))gizmo_.End(editor,false);
        if(viewport.colorTexture!=ImTextureID_Invalid)ImGui::Image(ImTextureRef(viewport.colorTexture),{rect.width,rect.height});
        else {ImGui::InvisibleButton("viewport-content",{rect.width,rect.height});ImGui::GetWindowDrawList()->AddRectFilled(origin,{origin.x+rect.width,origin.y+rect.height},IM_COL32(22,25,31,255));}
        const bool hovered=ImGui::IsItemHovered();const auto mouse=ImGui::GetMousePos();
        const Vec2 pointer{mouse.x,mouse.y};
        AssetId asset;
        if(!editor.IsPlaying()&&AcceptAsset(asset))
        {
            std::vector<AssetDiagnostic> diagnostics;
            if(assets.LoadModel(asset,diagnostics))Queue([&editor,asset,&text]{
                if(editor.BeginEdit("Editor.Mesh")){const auto id=editor.Create(text.GetText("EditorUi.NewEntity"));if(id&&editor.SetMesh(id,{asset,{}}))editor.CommitEdit();else editor.CancelEdit();}
            });
            else errorKey_="Assets.UnsupportedFormat";
        }
        bool hitGizmo=false;
        if(viewport.hasCamera&&!editor.IsPlaying()&&!inspectorEditing_)
        {
            const auto handle=editor.EditWorld().FindByStableId(editor.Selected());
            if(handle)
            {
                const auto world=editor.EditWorld().GetWorldMatrix(handle);
                const auto position=TransformPoint(world,{});const auto screen=Project(position,viewport.viewProjection,rect);
                if(screen)
                {
                    const ImU32 colors[]={IM_COL32(240,85,85,255),IM_COL32(100,220,115,255),IM_COL32(100,145,245,255)};
                    for(int index=0;index<3;++index)
                    {
                        const auto axis=static_cast<GizmoAxis>(index);
                        const auto direction=EditorGizmoAxis(editor.EditWorld(),handle,axis,space_);
                        const auto endpoint=Project(position+direction,viewport.viewProjection,rect);if(!endpoint)continue;
                        const float length=std::hypot(endpoint->x-screen->x,endpoint->y-screen->y);if(length<0.5f)continue;
                        const Vec2 unit{(endpoint->x-screen->x)/length,(endpoint->y-screen->y)/length};
                        const Vec2 end{screen->x+unit.x*70,screen->y+unit.y*70};
                        auto* draw=ImGui::GetWindowDrawList();draw->PushClipRect(origin,{origin.x+rect.width,origin.y+rect.height},true);
                        draw->AddLine({screen->x,screen->y},{end.x,end.y},colors[index],3);draw->AddCircleFilled({end.x,end.y},5,colors[index]);draw->PopClipRect();
                        if(hovered&&!gizmo_.Active()&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&DistanceToSegment(pointer,*screen,end)<7)
                        {hitGizmo=gizmo_.Begin(editor,mode_,axis,pointer,unit,mode_==GizmoMode::Translate?length:80,space_,dragCamera?&*dragCamera:nullptr);if(hitGizmo)break;errorKey_=gizmo_.LastError();}
                    }
                }
            }
        }
        if(gizmo_.Active())
        {
            if(ImGui::IsKeyPressed(ImGuiKey_Escape,false))gizmo_.End(editor,false);
            else if(ImGui::IsMouseDown(ImGuiMouseButton_Left)){if(!gizmo_.Update(editor,pointer)){errorKey_=gizmo_.LastError();gizmo_.End(editor,false);}}
            else gizmo_.End(editor,true);
        }
        else if(!editor.IsPlaying()&&!hitGizmo&&hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&viewport.pick)
        {const auto id=viewport.pick((mouse.x-rect.x)/rect.width,(mouse.y-rect.y)/rect.height);Queue([&editor,id]{editor.Select(id);});}
        if(viewport.colorTexture==ImTextureID_Invalid)ImGui::GetWindowDrawList()->AddText({rect.x+10,rect.y+10},IM_COL32(180,185,200,255),text.GetText("EditorUi.ViewportPending").c_str());
    }
    ImGui::End();return rect;
}

bool ImGuiEditor::UiChanged(UiTree& ui,std::string before)
{
    const auto after=ui.SaveJson();if(before==after)return true;
    if(!uiTransaction_.empty())return true;
    if(uiUndo_.size()==100)uiUndo_.erase(uiUndo_.begin());
    uiUndo_.push_back({std::move(before),after});uiRedo_.clear();return true;
}
bool ImGuiEditor::EditUi(UiTree& ui,const std::function<bool(UiTree&)>& operation)
{
    BindUi(ui);
    const auto before=ui.SaveJson();
    if(!operation||!operation(ui)){std::string error;ui.LoadJson(before,error);return false;}
    return UiChanged(ui,before);
}
void ImGuiEditor::BeginUiEdit(UiTree& ui){if(uiTransaction_.empty())uiTransaction_=ui.SaveJson();}
void ImGuiEditor::EndUiEdit(UiTree& ui,bool commit)
{
    if(uiTransaction_.empty())return;
    auto before=std::move(uiTransaction_);uiTransaction_.clear();
    if(commit)UiChanged(ui,std::move(before));else ui.LoadJson(before,errorKey_);
}
bool ImGuiEditor::UiUndo(UiTree& ui)
{
    BindUi(ui);
    if(uiUndo_.empty()||!uiTransaction_.empty())return false;
    if(!ui.LoadJson(uiUndo_.back().before,errorKey_))return false;
    if(!ui.Get(selectedUi_))selectedUi_=0;
    uiRedo_.push_back(std::move(uiUndo_.back()));uiUndo_.pop_back();return true;
}
bool ImGuiEditor::UiRedo(UiTree& ui)
{
    BindUi(ui);
    if(uiRedo_.empty()||!uiTransaction_.empty())return false;
    if(!ui.LoadJson(uiRedo_.back().after,errorKey_))return false;
    if(!ui.Get(selectedUi_))selectedUi_=0;
    uiUndo_.push_back(std::move(uiRedo_.back()));uiRedo_.pop_back();return true;
}

void ImGuiEditor::BindUi(UiTree& ui)
{
    if(uiOwner_==&ui)return;
    uiOwner_=&ui;selectedUi_=inspectedUi_=0;uiUndo_.clear();uiRedo_.clear();uiTransaction_.clear();uiSaved_.clear();uiDragging_=false;
    uiSavedPath_.clear();uiStamp_={};
    const auto path=asset_io::FromUtf8(uiPath_.data());std::vector<std::uint8_t> bytes;
    if(!path.empty()&&(projectRoot_.empty()||asset_io::Within(projectRoot_,path))&&asset_io::Read(path,bytes))
    {
        UiTree stored;std::string error;
        if(stored.LoadJson(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),error)&&stored.SaveJson()==ui.SaveJson())
        {uiSaved_=ui.SaveJson();uiSavedPath_=path;RecordStamp(uiStamp_,path);}
    }
    uiCanvas_.Reset();uiReferenceDraft_=uiReferenceObserved_=ui.CanvasReference();bindingNode_=0;
}
bool ImGuiEditor::SaveUi(UiTree& ui,const std::filesystem::path& path)
{
    BindUi(ui);
    if(!uiTransaction_.empty()){errorKey_="Editor.TransactionActive";return false;}
    if(!uiSavedPath_.empty()&&asset_io::Within(path,uiSavedPath_)&&asset_io::Within(uiSavedPath_,path)
        &&!CheckExternalDocument(ExternalDocument::Ui,path))return false;
    const bool sameSaved=!uiSavedPath_.empty()&&asset_io::Within(path,uiSavedPath_)&&asset_io::Within(uiSavedPath_,path);
    if(!sameSaved&&std::filesystem::exists(path))
    {externalDocument_=ExternalDocument::Ui;externalDocumentPath_=path;errorKey_="Editor.ExternalChange";return false;}
    const auto json=ui.SaveJson();
    if(!asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(json.data()),json.size()}))
    {errorKey_="EditorUi.UiWriteFailed";return false;}
    uiSaved_=json;uiSavedPath_=path;RecordStamp(uiStamp_,path);return true;
}
bool ImGuiEditor::OpenUi(UiTree& ui,const std::filesystem::path& path,bool discardUnsaved)
{
    BindUi(ui);
    if(!uiTransaction_.empty()){errorKey_="Editor.TransactionActive";return false;}
    if(!discardUnsaved&&(uiSaved_.empty()?ui.Count()!=0:ui.SaveJson()!=uiSaved_))
    {errorKey_="Editor.UnsavedChanges";return false;}
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)){errorKey_="EditorUi.UiReadFailed";return false;}
    if(!ui.LoadJson(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),errorKey_))return false;
    uiSaved_=ui.SaveJson();selectedUi_=inspectedUi_=0;uiUndo_.clear();uiRedo_.clear();uiSavedPath_=path;RecordStamp(uiStamp_,path);return true;
}

bool ImGuiEditor::AssignUiImage(UiTree& ui,AssetStore& assets,UiId node,const AssetId& asset)
{
    std::vector<AssetDiagnostic> diagnostics;
    if(!asset.empty()&&!assets.LoadTextureImage(asset,diagnostics))
    {errorKey_=diagnostics.empty()?"Assets.UnknownId":diagnostics.back().key;assetDiagnostics_=std::move(diagnostics);return false;}
    return EditUi(ui,[&](UiTree& tree){return tree.SetImage(node,asset);});
}
EditorRect ImGuiEditor::UiDesigner(UiTree& ui,AssetStore& assets,const Localization& text,const EditorViewport& viewport)
{
    BindUi(ui);EditorRect result{0,0,0,0};
    if(uiReferenceObserved_!=ui.CanvasReference())uiReferenceDraft_=uiReferenceObserved_=ui.CanvasReference();
    PreparePanel(EditorPanel::UiDesigner,150,100,850,550);
    if(ImGui::Begin(Label(text,"EditorUi.UiDesigner").c_str()))
    {
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))documentContext_=DocumentContext::Ui;
        ImGui::SetNextItemWidth(330);ImGui::InputText(Label(text,"EditorUi.UiPath").c_str(),uiPath_.data(),uiPath_.size());
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.SaveUi").c_str()))SaveUi(ui,asset_io::FromUtf8(uiPath_.data()));
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.OpenUi").c_str()))
        {if(!OpenUi(ui,asset_io::FromUtf8(uiPath_.data()))&&errorKey_=="Editor.UnsavedChanges")ImGui::OpenPopup("discard-ui");}
        if(ImGui::BeginPopup("discard-ui"))
        {
            ImGui::TextUnformatted(text.GetText("EditorUi.DiscardPrompt").c_str());
            if(ImGui::Button(Label(text,"EditorUi.DiscardOpen").c_str())){OpenUi(ui,asset_io::FromUtf8(uiPath_.data()),true);ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str()))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        if(ImGui::Button(Label(text,"EditorUi.UiUndo").c_str()))UiUndo(ui);
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.UiRedo").c_str()))UiRedo(ui);
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.CreateUi").c_str()))ImGui::OpenPopup("ui-create");
        if(ImGui::BeginPopup("ui-create"))
        {
            const char* keys[]={"EditorUi.Panel","EditorUi.Label","EditorUi.Image","EditorUi.Button","EditorUi.Checkbox","EditorUi.Slider","EditorUi.Progress","EditorUi.Scroll","EditorUi.TextInput"};
            for(int index=0;index<9;++index)if(ImGui::MenuItem(Label(text,keys[index]).c_str()))EditUi(ui,[&](UiTree& tree){selectedUi_=tree.Create(static_cast<UiKind>(index),selectedUi_);return selectedUi_!=0;});
            ImGui::EndPopup();
        }
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.DeleteUi").c_str()))EditUi(ui,[&](UiTree& tree){const bool ok=tree.Remove(selectedUi_);if(ok)selectedUi_=0;return ok;});
        if(ImGui::Checkbox(Label(text,"EditorUi.RuntimeCanvas").c_str(),&uiCanvasMode_)&&uiDragging_)
        {uiCanvas_.End(ui,false);EndUiEdit(ui,false);uiDragging_=false;}
        auto preview=uiCanvas_.PreviewSize();int dimensions[]{static_cast<int>(preview.x),static_cast<int>(preview.y)};
        ImGui::SameLine();ImGui::SetNextItemWidth(150);
        if(ImGui::InputInt2(Label(text,"EditorUi.PreviewSize").c_str(),dimensions))
        {if(!uiCanvas_.SetPreview({float(dimensions[0]),float(dimensions[1])}))errorKey_=uiCanvas_.LastError();}
        ImGui::SameLine();ImGui::SetNextItemWidth(130);int preset=-1;const char* presets[]{"640 x 360","1280 x 720","1920 x 1080"};
        if(ImGui::Combo(Label(text,"EditorUi.PreviewPreset").c_str(),&preset,presets,3))
        {const UiPoint sizes[]{{640,360},{1280,720},{1920,1080}};uiCanvas_.SetPreview(sizes[preset]);}
        ImGui::SetNextItemWidth(180);ImGui::InputFloat2(Label(text,"EditorUi.ReferenceSize").c_str(),&uiReferenceDraft_.x,"%.0f");
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.ApplyReference").c_str()))EditUi(ui,[&](UiTree& tree){return tree.SetCanvasReference(uiReferenceDraft_.x,uiReferenceDraft_.y);});
        if(ImGui::BeginTable("ui-layout",2,ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("ui-properties",ImGuiTableColumnFlags_WidthFixed,270);ImGui::TableSetupColumn("ui-canvas");ImGui::TableNextColumn();
            ImGui::BeginChild("ui-properties",{0,0});
            ImGui::BeginChild("ui-tree",{0,150},ImGuiChildFlags_Borders);
            for(auto id:ui.Ids())
            {
                const auto* node=ui.Get(id);if(!node)continue;
                ImGui::PushID(std::to_string(id).c_str());
                const std::string name=std::to_string(id)+" "+node->text;
                if(ImGui::Selectable(name.c_str(),selectedUi_==id))selectedUi_=id;
                if(ImGui::BeginDragDropSource()){ImGui::SetDragDropPayload("SE_UI_NODE",&id,sizeof(id));ImGui::TextUnformatted(name.c_str());ImGui::EndDragDropSource();}
                if(ImGui::BeginDragDropTarget())
                {
                    if(const auto* payload=ImGui::AcceptDragDropPayload("SE_UI_NODE"))if(payload->DataSize==sizeof(UiId))
                    {UiId moved;std::memcpy(&moved,payload->Data,sizeof(moved));EditUi(ui,[&](UiTree& tree){return tree.Reparent(moved,id);});}
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if(const auto* selected=ui.Get(selectedUi_))
            {
                const auto node=*selected;
                const bool changedSelection=inspectedUi_!=node.id;
                if(changedSelection||inspectedUiText_!=node.text){inspectedUi_=node.id;inspectedUiText_=node.text;CopyText(uiText_,node.text);}
                if(ImGui::InputText(Label(text,"EditorUi.Text").c_str(),uiText_.data(),uiText_.size(),ImGuiInputTextFlags_EnterReturnsTrue))EditUi(ui,[&](UiTree& tree){return tree.SetText(node.id,uiText_.data());});
                if(changedSelection||inspectedUiImage_!=node.imageAsset){inspectedUiImage_=node.imageAsset;CopyText(uiImage_,node.imageAsset);}
                if(ImGui::InputText(Label(text,"EditorUi.ImageAsset").c_str(),uiImage_.data(),uiImage_.size(),ImGuiInputTextFlags_EnterReturnsTrue))AssignUiImage(ui,assets,node.id,uiImage_.data());
                auto style=node.style;
                const auto editStyle=[&](bool changed){if(ImGui::IsItemActivated())BeginUiEdit(ui);if(changed)EditUi(ui,[&](UiTree& tree){return tree.SetStyle(node.id,style);});if(ImGui::IsItemDeactivated())EndUiEdit(ui,true);};
                editStyle(ImGui::DragFloat2(Label(text,"EditorUi.Offset").c_str(),&style.offset.x,1));
                editStyle(ImGui::DragFloat2(Label(text,"EditorUi.Size").c_str(),&style.size.x,1,0,100000));
                editStyle(ImGui::DragFloat2(Label(text,"EditorUi.AnchorMin").c_str(),&style.anchorMin.x,0.01f,0,1));
                editStyle(ImGui::DragFloat2(Label(text,"EditorUi.AnchorMax").c_str(),&style.anchorMax.x,0.01f,0,1));
                editStyle(ImGui::DragFloat2(Label(text,"EditorUi.Pivot").c_str(),&style.pivot.x,0.01f,0,1));
                editStyle(ImGui::DragFloat(Label(text,"EditorUi.Padding").c_str(),&style.padding,1,0,10000));
                editStyle(ImGui::DragFloat(Label(text,"EditorUi.Spacing").c_str(),&style.spacing,1,0,10000));
                int layout=static_cast<int>(style.layout);
                const auto absolute=text.GetText("EditorUi.Absolute"),horizontal=text.GetText("EditorUi.Horizontal"),vertical=text.GetText("EditorUi.Vertical");
                const char* layouts[]={absolute.c_str(),horizontal.c_str(),vertical.c_str()};
                if(ImGui::Combo(Label(text,"EditorUi.Layout").c_str(),&layout,layouts,3))
                {style.layout=static_cast<UiLayout>(layout);EditUi(ui,[&](UiTree& tree){return tree.SetStyle(node.id,style);});}
                if(ImGui::Checkbox(Label(text,"EditorUi.Clip").c_str(),&style.clip))EditUi(ui,[&](UiTree& tree){return tree.SetStyle(node.id,style);});
                double minimum=node.minimum,maximum=node.maximum,value=node.value;
                bool rangeChanged=ImGui::InputDouble(Label(text,"EditorUi.Minimum").c_str(),&minimum);
                rangeChanged|=ImGui::InputDouble(Label(text,"EditorUi.Maximum").c_str(),&maximum);
                if(rangeChanged)EditUi(ui,[&](UiTree& tree){return tree.SetRange(node.id,minimum,maximum);});
                if(ImGui::InputDouble(Label(text,"EditorUi.Value").c_str(),&value))EditUi(ui,[&](UiTree& tree){return tree.SetValue(node.id,value,false);});
                double step=node.step;
                const bool stepChanged=ImGui::InputDouble(Label(text,"EditorUi.Step").c_str(),&step,0,0,"%.6g");
                if(ImGui::IsItemActivated())BeginUiEdit(ui);if(stepChanged)EditUi(ui,[&](UiTree& tree){return tree.SetStep(node.id,step);});if(ImGui::IsItemDeactivated())EndUiEdit(ui,true);
                auto color=node.color;const bool colorChanged=ImGui::ColorEdit4(Label(text,"EditorUi.UiColor").c_str(),&color.r);
                if(ImGui::IsItemActivated())BeginUiEdit(ui);if(colorChanged)EditUi(ui,[&](UiTree& tree){return tree.SetColor(node.id,color);});if(ImGui::IsItemDeactivated())EndUiEdit(ui,true);
                float opacity=node.opacity;const bool opacityChanged=ImGui::SliderFloat(Label(text,"EditorUi.Opacity").c_str(),&opacity,0,1);
                if(ImGui::IsItemActivated())BeginUiEdit(ui);if(opacityChanged)EditUi(ui,[&](UiTree& tree){tree.FadeTo(node.id,opacity,0);return true;});if(ImGui::IsItemDeactivated())EndUiEdit(ui,true);
                bool visible=node.visible,enabled=node.enabled;
                if(ImGui::Checkbox(Label(text,"EditorUi.Visible").c_str(),&visible))EditUi(ui,[&](UiTree& tree){return tree.SetVisible(node.id,visible);});
                ImGui::SameLine();if(ImGui::Checkbox(Label(text,"EditorUi.Enabled").c_str(),&enabled))EditUi(ui,[&](UiTree& tree){return tree.SetEnabled(node.id,enabled);});
                bool blocking=node.blocksPointer;
                if(ImGui::Checkbox(Label(text,"EditorUi.PointerBlocking").c_str(),&blocking))EditUi(ui,[&](UiTree& tree){return tree.SetBlocksPointer(node.id,blocking);});
                ImGui::Button(Label(text,"EditorUi.DropImage").c_str(),{-1,25});AssetId asset;
                if(AcceptAsset(asset))AssignUiImage(ui,assets,node.id,asset);
                if(ImGui::Button(Label(text,"EditorUi.Unparent").c_str()))EditUi(ui,[&](UiTree& tree){return tree.Reparent(node.id,0);});
                ImGui::SeparatorText(text.GetText("EditorUi.Bindings").c_str());
                const std::string eventText[]{text.GetText("EditorUi.BindingClick"),text.GetText("EditorUi.BindingValue"),text.GetText("EditorUi.BindingText"),text.GetText("EditorUi.BindingFocus"),text.GetText("EditorUi.BindingEnter"),text.GetText("EditorUi.BindingLeave")};
                const char* eventNames[6];for(int i=0;i<6;++i)eventNames[i]=eventText[i].c_str();
                if(ImGui::Combo(Label(text,"EditorUi.BindingEvent").c_str(),&bindingEvent_,eventNames,6))bindingNode_=0;
                if(bindingNode_!=node.id)
                {
                    bindingNode_=node.id;bindingEntity_=0;uiBindingScript_.fill(0);uiBindingAction_.fill(0);
                    for(const auto& binding:node.bindings)if(static_cast<int>(binding.event)==bindingEvent_)
                    {bindingEntity_=binding.entity;CopyText(uiBindingScript_,binding.script);CopyText(uiBindingAction_,binding.action);break;}
                }
                ImGui::InputScalar(Label(text,"EditorUi.BindingEntity").c_str(),ImGuiDataType_U64,&bindingEntity_);
                ImGui::InputText(Label(text,"EditorUi.BindingScript").c_str(),uiBindingScript_.data(),uiBindingScript_.size());
                ImGui::InputText(Label(text,"EditorUi.BindingAction").c_str(),uiBindingAction_.data(),uiBindingAction_.size());
                if(ImGui::Button(Label(text,"EditorUi.BindingSet").c_str()))
                {
                    auto bindings=node.bindings;const UiEventBinding bindingValue{static_cast<UiEventKind>(bindingEvent_),bindingEntity_,uiBindingScript_.data(),uiBindingAction_.data()};
                    const auto existing=std::find_if(bindings.begin(),bindings.end(),[&](const auto& binding){return binding.event==bindingValue.event;});
                    if(existing==bindings.end())bindings.push_back(bindingValue);else *existing=bindingValue;
                    if(!EditUi(ui,[&](UiTree& tree){return tree.SetBindings(node.id,bindings);}))errorKey_="UI.InvalidBinding";
                }
                ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.BindingRemove").c_str()))
                {auto bindings=node.bindings;std::erase_if(bindings,[&](const auto& binding){return static_cast<int>(binding.event)==bindingEvent_;});EditUi(ui,[&](UiTree& tree){return tree.SetBindings(node.id,bindings);});bindingNode_=0;}
            }
            ImGui::EndChild();
            ImGui::TableNextColumn();
            const auto origin=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();
            if(!uiCanvasMode_)ImGui::TextWrapped("%s",text.GetText("EditorUi.CanvasEnableHint").c_str());
            else
            {
                const UiRect area{origin.x,origin.y,std::max(1.0f,size.x),std::max(1.0f,size.y)};
                if(uiCanvas_.Prepare(ui,area))
                {
                    result={area.x,area.y,area.width,area.height};
                    if(viewport.colorTexture!=ImTextureID_Invalid)ImGui::Image(ImTextureRef(viewport.colorTexture),{area.width,area.height});
                    else ImGui::InvisibleButton("ui-canvas",{area.width,area.height});
                    const bool hovered=ImGui::IsItemHovered();const auto mouse=ImGui::GetMousePos();const UiPoint point{mouse.x,mouse.y};
                    if(!uiDragging_&&!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape,false))selectedUi_=0;
                    if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        auto kind=UiCanvasDrag::Move;bool resize=false;
                        if(const auto anchors=uiCanvas_.Anchors(ui,selectedUi_))
                        {const auto r=anchors->rectangle;resize=UiRect{r.x+r.width-8,r.y+r.height-8,8,8}.Contains(point);}
                        if(resize)kind=UiCanvasDrag::Resize;else selectedUi_=uiCanvas_.Hit(ui,point);
                        if(selectedUi_)
                        {
                            if(uiCanvas_.Begin(ui,selectedUi_,kind,point)){BeginUiEdit(ui);uiDragging_=true;}
                            else errorKey_=uiCanvas_.LastError();
                        }
                    }
                    if(uiDragging_)
                    {
                        if(ImGui::IsKeyPressed(ImGuiKey_Escape,false)||ImGui::GetIO().AppFocusLost)
                        {uiCanvas_.End(ui,false);EndUiEdit(ui,false);uiDragging_=false;}
                        else if(ImGui::IsMouseDown(ImGuiMouseButton_Left))
                        {if(!uiCanvas_.Update(ui,point)){errorKey_=uiCanvas_.LastError();uiCanvas_.End(ui,false);EndUiEdit(ui,false);uiDragging_=false;}}
                        else {uiCanvas_.End(ui,true);EndUiEdit(ui,true);uiDragging_=false;}
                    }
                    uiCanvas_.Prepare(ui,area);auto* draw=ImGui::GetWindowDrawList();const auto content=uiCanvas_.Content();
                    draw->PushClipRect({content.x,content.y},{content.x+content.width,content.y+content.height},true);
                    draw->AddRect({content.x,content.y},{content.x+content.width,content.y+content.height},IM_COL32(110,120,140,255));
                    if(const auto anchors=uiCanvas_.Anchors(ui,selectedUi_))
                    {
                        const auto r=anchors->rectangle;draw->AddRect({r.x,r.y},{r.x+r.width,r.y+r.height},IM_COL32(255,195,80,255),0.0f,ImDrawFlags_None,2.0f);
                        draw->AddRectFilled({r.x+r.width-8,r.y+r.height-8},{r.x+r.width,r.y+r.height},IM_COL32(255,220,140,255));
                        const auto lo=anchors->minimum,hi=anchors->maximum,pivot=anchors->pivot;
                        draw->AddRect({lo.x,lo.y},{hi.x,hi.y},IM_COL32(90,220,140,255));
                        for(const auto p:{lo,hi}){draw->AddLine({p.x-5,p.y},{p.x+5,p.y},IM_COL32(90,220,140,255),2);draw->AddLine({p.x,p.y-5},{p.x,p.y+5},IM_COL32(90,220,140,255),2);}
                        draw->AddCircle({pivot.x,pivot.y},4,IM_COL32(230,120,220,255),0,2);
                    }
                    draw->PopClipRect();
                }
                else errorKey_=uiCanvas_.LastError();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();return result;
}
std::filesystem::path ImGuiEditor::EditorRoot() const
{
    const auto executableRoot=EditorSessionStore::EngineRootFromExecutable();
    if(!executableRoot.empty())return executableRoot;
    return projectRoot_;
}
std::filesystem::path ImGuiEditor::ProjectManifestPath() const
{
    std::error_code error;
    if(!projectRoot_.empty())return projectRoot_/"project.json";
    return EditorRoot()/"project.json";
}
bool ImGuiEditor::SaveAllDocuments(EditorSession& editor,AssetStore& assets,UiTree* ui)
{
    SyncDocumentRegistry(editor,ui);
    if(!externalDocumentsValid_){errorKey_="Editor.DocumentSnapshotFailed";LogConsole(2,errorKey_);return false;}
    std::vector<EditorAssetDocument> editingDocuments;std::vector<EditorAssetIssue> validation;
    if(!CaptureEditingDocuments(editor,assets,ui,editingDocuments))return false;
    if(!EditorAssetActions::ValidatePlayAssets(editor.EditWorld(),ui,assets,editingDocuments,validation))
    {for(const auto& issue:validation)LogConsole(2,issue.key,issue.detail);errorKey_=validation.empty()?"Assets.PlayValidationFailed":validation.front().key;return false;}
    const auto results=documents_.SaveAll([&](const EditorDocumentEntry& entry,std::string& key,std::string& detail){
        if(entry.id=="scene")
        {
            const auto action=files_.SaveCurrent(editor,asset_io::FromUtf8(sceneSavePath_.data()));
            if(action==FileActionResult::Done)return true;
            key=action==FileActionResult::Waiting?"Editor.SaveAllPending":"Editor.SaveAllFailed";
            detail=files_.LastError();return false;
        }
        if(entry.id=="material")
        {
            if(SaveMaterial(assets))return true;
            key=materialOverwrite_?"Editor.SaveAllPending":"Editor.SaveAllFailed";
            detail=errorKey_;return false;
        }
        if(entry.id=="ui")
        {
            if(ui&&uiOwner_==ui&&SaveUi(*ui,asset_io::FromUtf8(uiPath_.data())))return true;
            key="Editor.SaveAllFailed";detail=errorKey_;return false;
        }
        if(entry.id=="project-settings"||entry.id=="build-settings")
        {if(SaveSettings(entry.id=="project-settings"))return true;key=errorKey_;return false;}
        for(const auto& external:externalDocumentSnapshots_)
            if(external.document.documentId==entry.id&&externalDocuments_.save)
                return externalDocuments_.save(entry.id,key);
        key="Editor.CommandUnknown";detail=entry.id;return false;
    });
    bool success=true;std::string failureKey;
    for(const auto& result:results)
    {
        if(result.saved)continue;
        success=false;
        if(failureKey.empty())failureKey=result.diagnosticKey.empty()?"Editor.SaveAllFailed":result.diagnosticKey;
        // 注册表拦下外部变更时，把对应文档交给已有的外部变更弹窗处理。
        if(result.diagnosticKey=="Editor.ExternalChange")
        {
            if(result.id=="scene")files_.SaveCurrent(editor,asset_io::FromUtf8(sceneSavePath_.data()));
            else if(result.id=="material"){externalDocument_=ExternalDocument::Material;externalDocumentPath_=documents_.Find(result.id)->path;}
            else if(result.id=="ui"){externalDocument_=ExternalDocument::Ui;externalDocumentPath_=documents_.Find(result.id)->path;}
            else if(result.id=="project-settings"||result.id=="build-settings")
            {externalDocument_=result.id=="project-settings"?ExternalDocument::ProjectSettings:ExternalDocument::BuildSettings;externalDocumentPath_=documents_.Find(result.id)->path;}
        }
        LogConsole(2,result.diagnosticKey.empty()?"Editor.SaveAllFailed":result.diagnosticKey,result.detail);
    }
    if(!success)errorKey_=failureKey;
    return success;
}
bool ImGuiEditor::UiDirty(UiTree* ui) const
{
    if(!ui||uiOwner_!=ui)return false;
    if(uiSaved_.empty())return ui->Count()>0;
    return ui->SaveJson()!=uiSaved_;
}
void ImGuiEditor::RecordStamp(FileStamp& stamp,const std::filesystem::path& path) const
{
    std::error_code error;
    stamp={};
    if(path.empty())return;
    const auto size=std::filesystem::file_size(path,error);
    if(error)return;
    const auto time=std::filesystem::last_write_time(path,error);
    if(error)return;
    stamp.size=size;stamp.time=time;stamp.valid=true;
    std::vector<std::uint8_t> bytes;if(asset_io::Read(path,bytes))stamp.digest=asset_io::Hash(bytes);
}
bool ImGuiEditor::StampChanged(const FileStamp& stamp,const std::filesystem::path& path) const
{
    if(!stamp.valid||path.empty())return false;
    std::error_code error;
    const auto size=std::filesystem::file_size(path,error);
    if(error)return true;
    const auto time=std::filesystem::last_write_time(path,error);
    if(error||size!=stamp.size||time!=stamp.time)return true;
    std::vector<std::uint8_t> bytes;
    return !asset_io::Read(path,bytes)||(!stamp.digest.empty()&&asset_io::Hash(bytes)!=stamp.digest);
}
bool ImGuiEditor::CheckExternalDocument(ExternalDocument document,const std::filesystem::path& path)
{
    const auto& stamp=document==ExternalDocument::Material?materialStamp_:uiStamp_;
    if(!StampChanged(stamp,path))return true;
    externalDocument_=document;externalDocumentPath_=path;
    errorKey_="Editor.ExternalChange";
    return false;
}
void ImGuiEditor::SyncDocumentRegistry(EditorSession& editor,UiTree* ui)
{
    SyncSettingsDrafts();
    const auto sync=[this](const char* id,bool dirty,const std::filesystem::path& path){
        if(path.empty())return;
        const auto* entry=documents_.Find(id);
        if(!entry||entry->path!=path)
        {
            std::string error;documents_.Register(id,"document",path,error);
            entry=documents_.Find(id);
        }
        if(!entry)return;
        if(dirty&&entry->editRevision==entry->savedRevision)documents_.MarkEdited(id);
        else if(!dirty&&entry->editRevision!=entry->savedRevision)documents_.RevertTo(id,entry->savedRevision);
    };
    const auto scenePath=!editor.Path().empty()?editor.Path():asset_io::FromUtf8(sceneSavePath_.data());
    sync("scene",editor.Dirty(),scenePath);
    const auto materialPath=!activeMaterialPath_.empty()?activeMaterialPath_:asset_io::FromUtf8(materialPath_.data());
    sync("material",MaterialDirty(),materialPath);
    const auto uiPath=!uiSavedPath_.empty()?uiSavedPath_:asset_io::FromUtf8(uiPath_.data());
    sync("ui",UiDirty(ui),uiPath);
    if(projectSettingsDocument_.Loaded())sync("project-settings",projectSettingsDocument_.Dirty(),projectSettingsDocument_.Path());
    if(buildSettingsDocument_.Loaded())sync("build-settings",buildSettingsDocument_.Dirty(),buildSettingsDocument_.Path());
    if(externalDocuments_.snapshot)
    {
        std::string error;std::vector<EditorExternalDocument> snapshots;
        if(externalDocuments_.snapshot(snapshots,error))
        {
            externalDocumentsValid_=true;
            externalDocumentSnapshots_=std::move(snapshots);
            for(const auto& external:externalDocumentSnapshots_)
                sync(external.document.documentId.c_str(),external.dirty,projectRoot_/asset_io::FromUtf8(external.document.relativePath));
        }
        else{externalDocumentsValid_=false;errorKey_=error.empty()?"Editor.DocumentSnapshotFailed":error;}
    }
}
bool ImGuiEditor::OtherDocumentsDirty() const
{
    if(!externalDocumentsValid_)return true;
    for(const auto& id:documents_.DirtyDocuments())if(id!="scene")return true;
    return false;
}
bool ImGuiEditor::DocumentsDirty(UiTree* ui) const
{
    if(!externalDocumentsValid_)return true;
    if(MaterialDirty())return true;
    if(projectSettingsDocument_.Dirty()||buildSettingsDocument_.Dirty())return true;
    for(const auto& external:externalDocumentSnapshots_)if(external.dirty)return true;
    return UiDirty(ui);
}
void ImGuiEditor::LogConsole(int level,const std::string& key,const std::string& detail)
{
    consoleEntries_.push_back({level,key,detail});
    if(consoleEntries_.size()>500)consoleEntries_.erase(consoleEntries_.begin(),consoleEntries_.begin()+100);
}
bool ImGuiEditor::ExportBusy() const
{
    return exportJob_&&exportJob_->Running();
}
bool ImGuiEditor::ExportFinished() const
{
    return exportSnapshot_.finished&&!(exportJob_&&exportJob_->Running());
}
ExportJobSnapshot ImGuiEditor::ExportSnapshot() const
{
    return exportSnapshot_;
}
void ImGuiEditor::RequestCancelExport()
{
    if(exportJob_)exportJob_->Cancel();
}
bool ImGuiEditor::StartExport(const std::string& name,std::string& errorKey)
{
    errorKey.clear();
    if(ExportBusy()){errorKey="Editor.ExportBusy";return false;}
    if(name.empty()||name.size()>128||name.find_first_of("<>:\"/\\|?*")!=std::string::npos)
    {errorKey="Project.InvalidName";return false;}
    const auto root=EditorRoot();
    const auto manifest=ProjectManifestPath();
    std::error_code fileError;
    if(!std::filesystem::is_regular_file(manifest,fileError)){errorKey="Project.ManifestNotFound";return false;}
    SYSTEMTIME time{};GetLocalTime(&time);
    char stamp[64]{};
    std::snprintf(stamp,sizeof(stamp),"%04u%02u%02u-%02u%02u%02u-%llu",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,
        static_cast<unsigned long long>(++exportSequence_));
    const auto evidence=userOperationDirectory_/(std::string("export-")+stamp);
    ExportJobRequest request{manifest,name,evidence,workspaceGeneration_};
    if(!exportJob_)exportJob_=std::make_unique<ExportJob>();
    ExportService::Runner runner=[this,root,evidence](const std::vector<std::string>& arguments,std::string& output,const ExportService::Progress& progress)
    {
        std::vector<std::wstring> wide={L"-NoProfile",L"-ExecutionPolicy",L"Bypass",L"-File",(root/L"Scripts"/L"Export-Game.ps1").wstring(),L"-SkipLease",L"-SkipSmoke"};
        for(const auto& argument:arguments)wide.push_back(asset_io::FromUtf8(argument).wstring());
        std::string lastPhase;
        const auto tick=[&]{
            const auto report=ExportService::ReadReport(evidence);
            if(report.valid&&!report.phase.empty()&&report.phase!=lastPhase){lastPhase=report.phase;if(progress)progress(report.phase);}
        };
        ProcessRunResult result;
        const auto* cancel=exportJob_?&exportJob_->CancelFlag():nullptr;
        const bool started=RunEditorProcess(WindowsPowerShellPath(),wide,root,evidence/"editor-export.log",cancel,result,tick);
        output=result.output;
        if(!started)return -1;
        return result.cancelled?1:result.exitCode;
    };
    if(!exportJob_->Start(request,runner,errorKey))return false;
    exportSnapshot_=exportJob_->Snapshot();
    exportJoined_=false;
    lastLoggedOperation_=0;
    LogConsole(0,"Editor.ExportStarted",name+" -> "+evidence.string());
    return true;
}
void ImGuiEditor::PollExport(const Localization& text)
{
    if(!exportReportLoaded_)
    {
        exportReportLoaded_=true;
        ExportJobSnapshot loaded;std::string error;
        if(LoadExportSnapshot(userOperationDirectory_,loaded,error)&&loaded.finished)exportSnapshot_=loaded;
    }
    if(exportJob_)
    {
        const bool running=exportJob_->Running();
        exportSnapshot_=exportJob_->Snapshot();
        if(!running&&!exportJoined_)
        {
            exportJob_->Join();exportJoined_=true;
            std::string error;SaveExportSnapshot(userOperationDirectory_,exportSnapshot_,error);
        }
    }
    const auto& report=exportSnapshot_.report;
    if(report.operationId!=0&&report.operationId!=lastLoggedOperation_&&exportSnapshot_.finished)
    {
        lastLoggedOperation_=report.operationId;
        if(exportSnapshot_.cancelled||report.state==ExportOperationState::Cancelled)
            LogConsole(1,"Editor.ExportCancelled");
        else if(report.state==ExportOperationState::Succeeded)
            LogConsole(0,"Editor.ExportSucceeded",exportSnapshot_.outputPackage.string());
        else
            LogConsole(2,report.diagnosticKey.empty()?"Editor.ExportFailed":report.diagnosticKey,
                exportSnapshot_.detail.empty()?report.logPath:exportSnapshot_.detail);
        (void)text;
    }
}
void ImGuiEditor::SyncSettingsDrafts()
{
    std::string error;
    if(projectSettingsDocument_.Loaded()&&projectFieldChanges_)
    {
        auto settings=projectSettingsDocument_.Project();
        if(projectFieldChanges_&1)settings.name=projectNameField_.data();
        if(projectFieldChanges_&2)settings.contentRoot=contentRootField_.data();
        if(projectFieldChanges_&4)settings.entryScene=entrySceneField_.data();
        if(!projectSettingsDocument_.Set(settings,error))projectSettingsError_=error;else projectFieldChanges_=0;
    }
    if(buildSettingsDocument_.Loaded()&&buildFieldChanges_)
    {
        auto settings=buildSettingsDocument_.Build();
        if(buildFieldChanges_&1)settings.scriptsProject=scriptsProjectField_.data();
        if(buildFieldChanges_&2)settings.scriptType=scriptTypeField_.data();
        if(buildFieldChanges_&4)settings.bindings=bindingsField_.data();
        if(buildFieldChanges_&8)settings.entryUi=entryUiField_.data();
        if(!buildSettingsDocument_.Set(settings,error))buildSettingsError_=error;else buildFieldChanges_=0;
    }
}
bool ImGuiEditor::SaveSettings(bool project)
{
    SyncSettingsDrafts();auto& document=project?projectSettingsDocument_:buildSettingsDocument_;
    auto& error=project?projectSettingsError_:buildSettingsError_;std::string detail;
    if(!document.Save(error,detail))
    {
        errorKey_=error;LogConsole(2,error,detail);
        if(error=="Editor.ExternalChange")
        {externalDocument_=project?ExternalDocument::ProjectSettings:ExternalDocument::BuildSettings;externalDocumentPath_=document.Path();}
        return false;
    }
    if(project)projectSettings_=document.Project();else buildSettings_=document.Build();
    std::string ignored;documents_.MarkSaved(project?"project-settings":"build-settings",ignored);
    error.clear();errorKey_.clear();LogConsole(0,project?"Editor.ProjectSettingsSaved":"Editor.BuildSettingsSaved");return true;
}
void ImGuiEditor::SettingsReloadDialog(const Localization& text)
{
    ImGui::OpenPopup("settings-reload");
    if(ImGui::BeginPopupModal("settings-reload",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s",text.GetText("EditorUi.DiscardPrompt").c_str());bool reload=false;
        if(ImGui::Button(Label(text,"EditorUi.SaveAndOpen").c_str()))
        {reload=SaveSettings(settingsReload_==1);if(!reload&&externalDocument_!=ExternalDocument::None){settingsReload_=0;ImGui::CloseCurrentPopup();}}
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.DiscardChanges").c_str()))reload=true;
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){settingsReload_=0;ImGui::CloseCurrentPopup();}
        if(reload){if(settingsReload_==1)LoadProjectSettings(true);else LoadBuildSettings(true);settingsReload_=0;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
}
void ImGuiEditor::LoadProjectSettings(bool discardUnsaved)
{
    projectSettingsLoaded_=true;
    projectSettingsError_.clear();
    const auto manifest=ProjectManifestPath();
    std::string key;
    if(projectSettingsDocument_.Load(manifest,key,discardUnsaved))
    {
        const auto& settings=projectSettingsDocument_.Project();
        projectSettings_=settings;
        CopyText(projectNameField_,settings.name);CopyText(contentRootField_,settings.contentRoot);CopyText(entrySceneField_,settings.entryScene);
        projectSettingsStatus_.clear();
        projectFieldChanges_=0;
        std::string ignored;documents_.Register("project-settings","project-settings",manifest,ignored);documents_.MarkReloaded("project-settings",ignored);
    }
    else projectSettingsError_=key;
}
void ImGuiEditor::LoadBuildSettings(bool discardUnsaved)
{
    buildSettingsLoaded_=true;
    buildSettingsError_.clear();
    const auto buildJson=ProjectManifestPath().parent_path()/"build.json";
    std::string key;
    if(buildSettingsDocument_.Load(buildJson,key,discardUnsaved))
    {
        const auto& settings=buildSettingsDocument_.Build();
        buildSettings_=settings;
        CopyText(scriptsProjectField_,settings.scriptsProject);CopyText(scriptTypeField_,settings.scriptType);
        CopyText(bindingsField_,settings.bindings);CopyText(entryUiField_,settings.entryUi);
        buildSettingsStatus_.clear();
        buildFieldChanges_=0;
        std::string ignored;documents_.Register("build-settings","build-settings",buildJson,ignored);if(buildSettingsDocument_.SourceExists())documents_.MarkReloaded("build-settings",ignored);
        else buildSettingsError_="Project.BuildSettingsMissing";
    }
    else buildSettingsError_=key;
    if(buildNameField_[0]==0)
    {
        auto base=ProjectManifestPath().parent_path().filename().string();
        if(base.empty())base="Game";
        for(auto& character:base)if(!std::isalnum(static_cast<unsigned char>(character))&&character!='_'&&character!='-')character='_';
        CopyText(buildNameField_,base+"-Export");
    }
}
void ImGuiEditor::ProjectSettingsPanel(const Localization& text)
{
    if(!showProjectSettings_)return;
    if(!projectSettingsLoaded_)LoadProjectSettings();
    PreparePanel(EditorPanel::ProjectSettings,340,40,560,470);
    bool open=true;
    if(ImGui::Begin(Label(text,"EditorUi.ProjectSettings").c_str(),&open))
    {
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))documentContext_=DocumentContext::ProjectSettings;
        const auto manifest=ProjectManifestPath();
        const auto root=manifest.parent_path();
        ImGui::TextWrapped("%s: %s",text.GetText("EditorUi.ProjectPath").c_str(),asset_io::Utf8(root).c_str());
        ImGui::BeginDisabled(!projectSettingsDocument_.Loaded());
        if(ImGui::InputText(Label(text,"EditorUi.Name").c_str(),projectNameField_.data(),projectNameField_.size()))projectFieldChanges_|=1;
        if(ImGui::InputText(Label(text,"EditorUi.ContentRoot").c_str(),contentRootField_.data(),contentRootField_.size()))projectFieldChanges_|=2;
        if(ImGui::InputText(Label(text,"EditorUi.EntryScene").c_str(),entrySceneField_.data(),entrySceneField_.size()))projectFieldChanges_|=4;
        ImGui::Separator();
        ImGui::TextUnformatted(text.GetText("EditorUi.AssetsReadOnly").c_str());
        ImGui::BeginChild("project-assets",{0,110},ImGuiChildFlags_Borders);
        if(projectSettings_.assets.empty())ImGui::TextUnformatted(text.GetText("EditorUi.NoAssets").c_str());
        for(const auto& asset:projectSettings_.assets)ImGui::BulletText("%s",asset.c_str());
        ImGui::EndChild();
        if(ImGui::Button(Label(text,"EditorUi.SaveProjectSettings").c_str()))
        {
            SaveSettings(true);
        }
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Reload").c_str())){SyncSettingsDrafts();if(projectSettingsDocument_.Dirty())settingsReload_=1;else LoadProjectSettings();}
        if(!projectSettingsError_.empty())ImGui::TextColored({0.95f,0.45f,0.4f,1}, "%s",text.GetText(projectSettingsError_.c_str()).c_str());
        else ImGui::TextUnformatted(text.GetText("EditorUi.SettingsReady").c_str());
    }
    ImGui::End();
    if(!open){showProjectSettings_=false;SaveLayout();}
}
void ImGuiEditor::BuildExportPanel(EditorSession& editor,AssetStore& assets,const Localization& text,UiTree* ui)
{
    if(!showBuild_)return;
    if(!buildSettingsLoaded_)LoadBuildSettings();
    PreparePanel(EditorPanel::BuildExport,340,60,620,560);
    bool open=true;
    if(ImGui::Begin(Label(text,"EditorUi.BuildExport").c_str(),&open))
    {
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))documentContext_=DocumentContext::BuildSettings;
        const auto root=ProjectManifestPath().parent_path();
        ImGui::TextUnformatted(text.GetText("EditorUi.BuildSettings").c_str());
        ImGui::BeginDisabled(!buildSettingsDocument_.Loaded());
        if(ImGui::InputText(Label(text,"EditorUi.ScriptsProject").c_str(),scriptsProjectField_.data(),scriptsProjectField_.size()))buildFieldChanges_|=1;
        if(ImGui::InputText(Label(text,"EditorUi.ScriptType").c_str(),scriptTypeField_.data(),scriptTypeField_.size()))buildFieldChanges_|=2;
        if(ImGui::InputText(Label(text,"EditorUi.BindingsFile").c_str(),bindingsField_.data(),bindingsField_.size()))buildFieldChanges_|=4;
        if(ImGui::InputText(Label(text,"EditorUi.EntryUi").c_str(),entryUiField_.data(),entryUiField_.size()))buildFieldChanges_|=8;
        if(ImGui::Button(Label(text,"EditorUi.SaveBuildSettings").c_str()))
        {
            SaveSettings(false);
        }
        ImGui::SameLine();
        if(ImGui::Button(Label(text,"EditorUi.CreateBuildSettings").c_str()))
        {
            SaveSettings(false);
        }
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Reload").c_str())){SyncSettingsDrafts();if(buildSettingsDocument_.Dirty())settingsReload_=2;else LoadBuildSettings();}
        if(!buildSettingsError_.empty())
        {
            const auto message=buildSettingsError_=="Project.BuildSettingsMissing"?text.GetText("Project.BuildSettingsMissing"):text.GetText(buildSettingsError_.c_str());
            ImGui::TextColored({0.95f,0.75f,0.35f,1},"%s",message.c_str());
        }
        ImGui::Separator();
        ImGui::TextUnformatted(text.GetText("EditorUi.ExportSection").c_str());
        ImGui::InputText(Label(text,"EditorUi.ExportName").c_str(),buildNameField_.data(),buildNameField_.size());
        ImGui::TextUnformatted(text.GetText("EditorUi.ExportTarget").c_str());
        const auto busy=ExportBusy();
        const auto dirty=DocumentsDirty(ui)||editor.Dirty();
        ImGui::BeginDisabled(busy||editor.IsPlaying());
        if(editor.IsPlaying())ImGui::TextColored({0.95f,0.75f,0.35f,1},"%s",text.GetText("Editor.ExportStopPlaying").c_str());
        else if(dirty)ImGui::TextColored({0.95f,0.75f,0.35f,1},"%s",text.GetText("Editor.ExportSaveFirst").c_str());
        if(ImGui::Button(Label(text,"EditorUi.ExportStart").c_str()))
        {
            if(dirty&&!SaveAllDocuments(editor,assets,ui)){errorKey_="Editor.SaveAllFailed";}
            else
            {
                std::string key;
                if(StartExport(buildNameField_.data(),key)){errorKey_.clear();}
                else errorKey_=key;
            }
        }
        ImGui::EndDisabled();
        if(busy){ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.ExportCancel").c_str()))RequestCancelExport();}
        const auto& report=exportSnapshot_.report;
        if(report.operationId!=0)
        {
            ImGui::Separator();
            ImGui::Text("%s: %s",text.GetText("EditorUi.ExportState").c_str(),
                text.GetText(report.state==ExportOperationState::Queued?"Editor.ExportState.Queued":
                    report.state==ExportOperationState::Running?"Editor.ExportState.Running":
                    report.state==ExportOperationState::Cancelling?"Editor.ExportState.Cancelling":
                    report.state==ExportOperationState::Succeeded?"Editor.ExportState.Succeeded":
                    report.state==ExportOperationState::Failed?"Editor.ExportState.Failed":"Editor.ExportState.Cancelled").c_str());
            ImGui::Text("%s: %s",text.GetText("EditorUi.ExportStage").c_str(),
                text.GetText(report.stage.empty()?"Editor.ExportStage.Work":report.stage.c_str()).c_str());
            if(report.exitCode!=0)ImGui::Text("%s: %d",text.GetText("EditorUi.ExportExitCode").c_str(),report.exitCode);
            if(!report.diagnosticKey.empty())
                ImGui::TextColored({0.95f,0.45f,0.4f,1},"%s",text.GetText(report.diagnosticKey).c_str());
            if(!exportSnapshot_.detail.empty())ImGui::TextWrapped("%s",exportSnapshot_.detail.c_str());
            if(!exportSnapshot_.outputPackage.empty())
            {
                ImGui::TextWrapped("%s: %s",text.GetText("EditorUi.ExportOutput").c_str(),asset_io::Utf8(exportSnapshot_.outputPackage).c_str());
                if(ImGui::Button(Label(text,"EditorUi.ExportOpenOutput").c_str()))OpenInExplorer(exportSnapshot_.outputPackage);
                ImGui::SameLine();
                if(ImGui::Button(Label(text,"EditorUi.ExportRun").c_str()))
                {
                    std::string error;
                    const auto script=exportSnapshot_.outputPackage/"Start-Game.ps1";
                    if(!StartEditorProcess(WindowsPowerShellPath(),{L"-NoProfile",L"-ExecutionPolicy",L"Bypass",L"-File",script.wstring()},
                        exportSnapshot_.outputPackage,error))errorKey_=error;
                    else LogConsole(0,"Editor.ExportGameStarted",exportSnapshot_.outputPackage.string());
                }
            }
            if(!exportSnapshot_.logPath.empty())
            {
                if(ImGui::Button(Label(text,"EditorUi.ExportLog").c_str()))OpenInExplorer(exportSnapshot_.logPath,true);
                ImGui::SameLine();ImGui::TextWrapped("%s",asset_io::Utf8(exportSnapshot_.logPath).c_str());
            }
        }
        else ImGui::TextUnformatted(text.GetText("EditorUi.ExportNoReport").c_str());
    }
    ImGui::End();
    if(!open){showBuild_=false;SaveLayout();}
}
void ImGuiEditor::ConsolePanel(const Localization& text)
{
    if(!showConsole_)return;
    PreparePanel(EditorPanel::Console,766,440,514,280);
    bool open=true;
    if(ImGui::Begin(Label(text,"EditorUi.Console").c_str(),&open))
    {
        const char* levels[]={"EditorUi.ConsoleAll","EditorUi.ConsoleInfo","EditorUi.ConsoleWarning","EditorUi.ConsoleError"};
        ImGui::SetNextItemWidth(160);
        if(ImGui::BeginCombo(Label(text,"EditorUi.ConsoleLevel").c_str(),text.GetText(levels[std::clamp(consoleLevelFilter_,0,3)]).c_str()))
        {
            for(int index=0;index<4;++index)
                if(ImGui::Selectable(text.GetText(levels[index]).c_str(),consoleLevelFilter_==index))consoleLevelFilter_=index;
            ImGui::EndCombo();
        }
        ImGui::SameLine();ImGui::SetNextItemWidth(260);
        ImGui::InputText(Label(text,"EditorUi.ConsoleSearch").c_str(),consoleSearch_.data(),consoleSearch_.size());
        ImGui::SameLine();ImGui::Checkbox(Label(text,"EditorUi.ConsoleAutoScroll").c_str(),&consoleAutoScroll_);
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.ConsoleClear").c_str()))consoleEntries_.clear();
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.ConsoleCopy").c_str()))
        {
            std::string clipboard;
            for(const auto& entry:consoleEntries_)
            {
                if(consoleLevelFilter_>0&&entry.level!=consoleLevelFilter_-1)continue;
                clipboard+=entry.key;if(!entry.detail.empty())clipboard+=": "+entry.detail;clipboard+='\n';
            }
            ImGui::SetClipboardText(clipboard.c_str());
        }
        ImGui::Separator();
        ImGui::BeginChild("console-entries");
        const std::string search=consoleSearch_.data();
        for(const auto& entry:consoleEntries_)
        {
            if(consoleLevelFilter_>0&&entry.level!=consoleLevelFilter_-1)continue;
            const auto line=entry.key+(entry.detail.empty()?std::string{}:": "+entry.detail);
            if(!search.empty()&&line.find(search)==std::string::npos)continue;
            const auto color=entry.level==2?ImVec4{0.95f,0.45f,0.4f,1}:entry.level==1?ImVec4{0.95f,0.75f,0.35f,1}:ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(color,"%s",text.GetText(line.c_str()).c_str());
        }
        if(consoleAutoScroll_&&ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-1)ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
    if(!open){showConsole_=false;SaveLayout();}
}
void ImGuiEditor::AboutPanel(const Localization& text)
{
    ImGui::SetNextWindowSize({520,260},ImGuiCond_FirstUseEver);
    if(ImGui::Begin(Label(text,"EditorUi.About").c_str(),&showAbout_,ImGuiWindowFlags_NoCollapse))
    {
#ifdef NDEBUG
        const char* configuration="Release";
#else
        const char* configuration="Debug";
#endif
        ImGui::Text("%s: %s",text.GetText("EditorUi.BuildConfiguration").c_str(),configuration);
        ImGui::TextWrapped("%s: %s",text.GetText("EditorUi.EngineRoot").c_str(),asset_io::Utf8(EditorRoot()).c_str());
        ImGui::TextWrapped("%s: %s",text.GetText("EditorUi.ProjectPath").c_str(),asset_io::Utf8(projectRoot_).c_str());
        ImGui::TextWrapped("%s: %s",text.GetText("EditorUi.UserData").c_str(),asset_io::Utf8(EditorSessionStore::UserDataDirectory()).c_str());
        ImGui::TextWrapped("%s: %s",text.GetText("EditorUi.ExportReport").c_str(),asset_io::Utf8(userOperationDirectory_/"last-export.json").c_str());
        if(ImGui::Button(Label(text,"EditorUi.Close").c_str()))showAbout_=false;
    }
    ImGui::End();
}
void ImGuiEditor::RunEnvironmentDiagnostics(const Localization& text)
{
    const auto root=EditorRoot();
    const auto executable=GetExecutableDirectory();
    std::error_code error;
    const auto status=[&](bool ok){return std::string(text.GetText(ok?"Editor.Diag.Ok":"Editor.Diag.Missing"));};
    std::string report;
    report+=text.GetText("Editor.Diag.Runtime")+":\n";
    report+="  EngineNative.dll: "+status(std::filesystem::is_regular_file(executable/"EngineNative.dll",error))+"\n";
    report+="  "+asset_io::Utf8(root/"Resources/Localization/zh-CN.txt")+": "+status(std::filesystem::is_regular_file(root/"Resources/Localization/zh-CN.txt",error))+"\n";
    report+="  "+asset_io::Utf8(root/"Resources/Templates/EmptyGame/project.json")+": "+status(std::filesystem::is_regular_file(root/"Resources/Templates/EmptyGame/project.json",error))+"\n";
    report+=text.GetText("Editor.Diag.Sdk")+":\n";
    wchar_t dotnetPath[MAX_PATH]{};
    const bool dotnetFound=SearchPathW(nullptr,L"dotnet.exe",nullptr,MAX_PATH,dotnetPath,nullptr)>0;
    report+="  dotnet.exe: "+status(dotnetFound)+(dotnetFound?(" ("+asset_io::Utf8(std::filesystem::path(dotnetPath))+")"):"")+"\n";
    report+="  "+asset_io::Utf8(root/"Scripts/Export-Game.ps1")+": "+status(std::filesystem::is_regular_file(root/"Scripts/Export-Game.ps1",error))+"\n";
    report+=text.GetText("Editor.Diag.Toolchain")+":\n";
    report+="  "+asset_io::Utf8(root/"out/build/windows-x64-release/ProjectTool.exe")+": "+status(std::filesystem::is_regular_file(root/"out/build/windows-x64-release/ProjectTool.exe",error))+"\n";
    report+="  "+asset_io::Utf8(root/"CMakeLists.txt")+": "+status(std::filesystem::is_regular_file(root/"CMakeLists.txt",error))+"\n";
    diagnosticsReport_=report;
    LogConsole(0,"Editor.DiagnosticsDone");
}
void ImGuiEditor::DiagnosticsPanel(const Localization& text)
{
    ImGui::SetNextWindowSize({680,420},ImGuiCond_FirstUseEver);
    if(ImGui::Begin(Label(text,"EditorUi.Diagnostics").c_str(),&showDiagnostics_,ImGuiWindowFlags_NoCollapse))
    {
        if(ImGui::Button(Label(text,"EditorUi.DiagnosticsRun").c_str()))RunEnvironmentDiagnostics(text);
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.ConsoleCopy").c_str()))ImGui::SetClipboardText(diagnosticsReport_.c_str());
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.OpenLogDirectory").c_str()))OpenInExplorer(userOperationDirectory_);
        ImGui::Separator();
        ImGui::BeginChild("diagnostics-report");
        ImGui::TextWrapped("%s",diagnosticsReport_.c_str());
        ImGui::EndChild();
    }
    ImGui::End();
}
void ImGuiEditor::OpenInExplorer(const std::filesystem::path& path,bool selectFile) const
{
    if(path.empty())return;
    std::error_code error;
    if(selectFile)
    {
        if(!std::filesystem::exists(path,error))return;
        const auto arguments=L"/select,\""+path.wstring()+L"\"";
        ShellExecuteW(nullptr,L"open",L"explorer.exe",arguments.c_str(),nullptr,SW_SHOWNORMAL);
        return;
    }
    if(!std::filesystem::exists(path,error))return;
    ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}
}
