#include "ToolLayer/Editor/ImGuiEditor.h"
#include "ToolLayer/Editor/ImGuiGeometry.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include <imgui/imgui_internal.h>
#include <cmath>
#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)
static bool Near(float a,float b){return std::abs(a-b)<0.0001f;}
int main(int argc,char** argv)
{
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_CALL_REPORTFAULT);
#endif
    const auto directory=std::filesystem::path(argc>1?argv[1]:".cache/validation/editor-ui")/se::asset_io::NewId();
    std::filesystem::create_directories(directory);
    se::EditorSession editor;
    const auto parent=editor.Create("Parent"),child=editor.Create("Child",parent);
    se::Transform parentTransform;parentTransform.scale={2,2,2};
    CHECK(editor.SetTransform(parent,parentTransform)&&editor.Select(child));
    const auto before=editor.UndoCount();
    se::EditorGizmo gizmo;
    CHECK(!gizmo.Begin(editor,se::GizmoMode::Translate,se::GizmoAxis::X,{},{0,0}));
    CHECK(gizmo.Begin(editor,se::GizmoMode::Translate,se::GizmoAxis::X,{0,0},{1,0}));
    CHECK(gizmo.Update(editor,{40,0})&&gizmo.Update(editor,{80,0}));
    CHECK(gizmo.End(editor)&&editor.UndoCount()==before+1);
    CHECK(Near(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child))->position.x,0.5f));
    CHECK(editor.Undo());
    CHECK(Near(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child))->position.x,0));
    CHECK(editor.Redo());
    CHECK(gizmo.Begin(editor,se::GizmoMode::Rotate,se::GizmoAxis::Z,{0,0},{1,0}));
    CHECK(gizmo.Update(editor,{80*se::Pi/2,0}));
    const auto rotation=editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child))->rotation;
    CHECK(Near(se::Rotate(rotation,{1,0,0}).y,1));
    CHECK(gizmo.End(editor,false));
    CHECK(Near(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child))->rotation.w,1));
    CHECK(gizmo.Begin(editor,se::GizmoMode::Scale,se::GizmoAxis::Y,{0,0},{0,1}));
    CHECK(gizmo.Update(editor,{0,80})&&gizmo.End(editor));
    CHECK(Near(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child))->scale.y,2));
    CHECK(editor.StartPlay());
    CHECK(!gizmo.Begin(editor,se::GizmoMode::Translate,se::GizmoAxis::X,{},{1,0}));
    editor.StopPlay();

    se::ImGuiEditor frontend;
    CHECK(!frontend.LoadFont(directory/"missing-font.ttf"));
    const unsigned char* pixels=nullptr;int width=0,height=0;
    frontend.FontAtlasRgba(pixels,width,height);
    CHECK(pixels&&width>0&&height>0);
    frontend.SetFontTexture(1); // 测试用不透明标识；不创建任何 GPU 对象。
    frontend.SetAuxiliaryPanels(true);
    se::AssetStore assets(directory,directory/"cache");
    Localization localization;
    se::EditorInputFrame input;
    se::EditorViewport viewport;viewport.hasCamera=true;
    bool picked=false;viewport.pick=[&](float,float){picked=true;return child;};
    se::UiTree ui;const auto panel=ui.Create(se::UiKind::Panel);
    const auto button=ui.Create(se::UiKind::Button,panel);CHECK(ui.SetText(button,"Button"));
    CHECK(frontend.EditUi(ui,[&](se::UiTree& tree){return tree.SetText(button,"Changed");}));
    CHECK(frontend.UiUndo(ui)&&ui.Get(button)->text=="Button");
    CHECK(frontend.UiRedo(ui)&&ui.Get(button)->text=="Changed");
    CHECK(frontend.SaveUi(ui,directory/"layout.json"));
    CHECK(frontend.EditUi(ui,[&](se::UiTree& tree){return tree.Remove(button);}));
    CHECK(!frontend.OpenUi(ui,directory/"layout.json"));
    CHECK(frontend.OpenUi(ui,directory/"layout.json",true)&&ui.Get(button));
    for(int frame=0;frame<3;++frame)
    {
        const auto result=frontend.Draw(editor,assets,localization,input,viewport,&ui);
        CHECK(result.drawData&&result.drawData->Valid&&result.drawData->TotalVtxCount>0);
        CHECK(result.viewport.width>0&&result.viewport.height>0);
        se::GuiGeometry geometry;CHECK(se::CopyImGuiGeometry(*result.drawData,geometry));
        CHECK(geometry.vertices.size()==static_cast<size_t>(result.drawData->TotalVtxCount)&&geometry.indices.size()==static_cast<size_t>(result.drawData->TotalIdxCount));
        if(frame==0)
        {
            auto* list=result.drawData->CmdLists[0];const auto commands=list->CmdBuffer.Size;
            list->AddCallback([](const ImDrawList*,const ImDrawCmd*){},nullptr);
            CHECK(!se::CopyImGuiGeometry(*result.drawData,geometry));list->CmdBuffer.resize(commands);
            list->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
            CHECK(se::CopyImGuiGeometry(*result.drawData,geometry));list->CmdBuffer.resize(commands);
            const auto offset=list->CmdBuffer[0].VtxOffset;list->CmdBuffer[0].VtxOffset=UINT32_MAX;
            CHECK(!se::CopyImGuiGeometry(*result.drawData,geometry));list->CmdBuffer[0].VtxOffset=offset;
            CHECK(se::CopyImGuiGeometry(*result.drawData,geometry));
            const auto first=geometry.vertices[0];const auto source=list->VtxBuffer[0].pos;list->VtxBuffer[0].pos.x+=1;
            CHECK(geometry.vertices[0].position.x==first.position.x);list->VtxBuffer[0].pos=source;
        }
    }
    // 没有鼠标点击时不执行 CPU 拾取；失焦不会遗留编辑事务。
    CHECK(!picked);
    // 改变实际 ImGui 树节点存储，确认折叠隐藏子行、展开恢复，稳定 ID 不改变选择。
    const auto selectedBefore=editor.Selected();auto* previousContext=ImGui::GetCurrentContext();ImGui::SetCurrentContext(frontend.Context());
    auto* hierarchyWindow=ImGui::FindWindowByID(ImHashStr("###EditorUi.Hierarchy"));CHECK(hierarchyWindow);
    const auto parentSeed=ImHashStr(std::to_string(parent).c_str(),0,hierarchyWindow->ID);const auto nodeId=ImHashStr("entity",0,parentSeed);
    hierarchyWindow->StateStorage.SetInt(nodeId,0);ImGui::SetCurrentContext(previousContext);
    const auto closed=frontend.Draw(editor,assets,localization,input,viewport,&ui);const auto closedVertices=closed.drawData->TotalVtxCount;
    ImGui::SetCurrentContext(frontend.Context());hierarchyWindow->StateStorage.SetInt(nodeId,1);ImGui::SetCurrentContext(previousContext);
    const auto opened=frontend.Draw(editor,assets,localization,input,viewport,&ui);
    CHECK(opened.drawData->TotalVtxCount>closedVertices&&editor.Selected()==selectedBefore);
    input.focused=false;frontend.Draw(editor,assets,localization,input,viewport,&ui);
    frontend.CancelInteractions(editor,&ui);
    CHECK(editor.StartPlay());editor.StopPlay();
    se::UiTree crowded;
    for(int index=0;index<8000;++index)
    {
        const auto id=crowded.Create(se::UiKind::Label);
        CHECK(id&&crowded.SetText(id,std::string(512,'W')));
        se::UiStyle style;style.offset={4,4};style.size={200,32};CHECK(crowded.SetStyle(id,style));
    }
    input.focused=true;
    for(int frame=0;frame<2;++frame)
    {
        const auto result=frontend.Draw(editor,assets,localization,input,viewport,&crowded);
        CHECK(result.drawData&&result.drawData->Valid);
        for(const auto* list:result.drawData->CmdLists)CHECK(list->VtxBuffer.Size<65536);
    }
    for(auto id:crowded.Ids()){auto style=crowded.Get(id)->style;style.offset={10000,10000};CHECK(crowded.SetStyle(id,style));}
    const auto culled=frontend.Draw(editor,assets,localization,input,viewport,&crowded);
    CHECK(culled.drawData&&culled.drawData->TotalVtxCount<20000);
    // 通过实际控件激活打开、编辑、赋材质、撤销/重做和保存，核对帧预览契约。
    se::MaterialDocument material;material.name="UI workflow";se::AssetId materialId;std::vector<se::AssetDiagnostic> diagnostics;
    CHECK(assets.SaveMaterialSource("Materials/Default.material.json",material,materialId,diagnostics));
    const auto uiBeforeInvalidImage=ui.SaveJson();
    CHECK(!frontend.AssignUiImage(ui,assets,button,materialId)&&ui.SaveJson()==uiBeforeInvalidImage);
    CHECK(!frontend.AssignUiImage(ui,assets,button,se::BuiltinCubeAssetId)&&ui.SaveJson()==uiBeforeInvalidImage);
    const auto worldSelectionBeforeLocate=editor.Selected();
    frontend.LocateAssetReference(editor,assets,{{},{},"/nodes/1/image",button,true,se::EditorAssetDocumentKind::Ui});
    CHECK(frontend.SelectedUi()==button&&editor.Selected()==worldSelectionBeforeLocate);
    frontend.SetUiCanvasEnabled(false);
    frontend.SetAuxiliaryPanels(true);
    CHECK(editor.SetMesh(child,{se::BuiltinCubeAssetId,{}})&&editor.Select(child));
    const auto activate=[&](const char* item){
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(frontend.Context());
        auto* window=ImGui::FindWindowByID(ImHashStr("###EditorUi.MaterialEditor"));
        if(window){ImGui::FocusWindow(window);ImGui::ActivateItemByID(window->GetID(item));}
        ImGui::SetCurrentContext(previous);return window!=nullptr;
    };
    CHECK(activate("###EditorUi.OpenMaterial"));frontend.Draw(editor,assets,localization,input,viewport,&ui);
    CHECK(!frontend.CurrentMaterialPreview());
    CHECK(activate("###material.doubleSided"));frontend.Draw(editor,assets,localization,input,viewport,&ui);
    const auto preview=frontend.CurrentMaterialPreview();CHECK(preview&&preview->id==materialId&&preview->document.doubleSided);
    se::MaterialDocument disk;CHECK(se::LoadMaterialDocument(directory/"Materials/Default.material.json",disk,diagnostics)&&!disk.doubleSided);
    CHECK(activate("###EditorUi.AssignMaterial"));frontend.Draw(editor,assets,localization,input,viewport,&ui);
    CHECK(editor.EditWorld().GetMesh(editor.EditWorld().FindByStableId(child))->materials==std::vector<std::string>{materialId});
    CHECK(activate("###EditorUi.Undo"));frontend.Draw(editor,assets,localization,input,viewport,&ui);CHECK(!frontend.CurrentMaterialPreview());
    CHECK(activate("###EditorUi.Redo"));frontend.Draw(editor,assets,localization,input,viewport,&ui);CHECK(frontend.CurrentMaterialPreview());
    CHECK(activate("###EditorUi.SaveMaterial"));frontend.Draw(editor,assets,localization,input,viewport,&ui);
    CHECK(!frontend.CurrentMaterialPreview()&&se::LoadMaterialDocument(directory/"Materials/Default.material.json",disk,diagnostics)&&disk.doubleSided);
    // 从材质切到 UI 后，通用快捷键必须操作 UI 文档。
    const auto uiBeforeShortcut=ui.Get(button)->text;
    CHECK(frontend.EditUi(ui,[&](se::UiTree& tree){return tree.SetText(button,"Canvas shortcut");}));frontend.SetUiCanvasEnabled(true);
    {
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(frontend.Context());auto* window=ImGui::FindWindowByID(ImHashStr("###EditorUi.UiDesigner"));CHECK(window);ImGui::ClearActiveID();ImGui::FocusWindow(window);ImGui::SetCurrentContext(previous);
    }
    input.keyEvents.clear();frontend.Draw(editor,assets,localization,input,viewport,&ui);
    input.keyEvents={{static_cast<ImGuiKey>(ImGuiMod_Ctrl),true},{ImGuiKey_Z,true}};frontend.Draw(editor,assets,localization,input,viewport,&ui);
    CHECK(ui.Get(button)->text==uiBeforeShortcut&&!frontend.CurrentMaterialPreview());
    input.keyEvents={{ImGuiKey_Z,false},{static_cast<ImGuiKey>(ImGuiMod_Ctrl),false}};frontend.Draw(editor,assets,localization,input,viewport,&ui);
    input.keyEvents={{static_cast<ImGuiKey>(ImGuiMod_Ctrl),true},{ImGuiKey_Y,true}};frontend.Draw(editor,assets,localization,input,viewport,&ui);
    CHECK(ui.Get(button)->text=="Canvas shortcut"&&!frontend.CurrentMaterialPreview());
    input.keyEvents={{ImGuiKey_Y,false},{static_cast<ImGuiKey>(ImGuiMod_Ctrl),false}};frontend.Draw(editor,assets,localization,input,viewport,&ui);
    const auto worldBeforeShortcut=*editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child));auto worldAfterShortcut=worldBeforeShortcut;worldAfterShortcut.position.y+=1;CHECK(editor.SetTransform(child,worldAfterShortcut));
    {
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(frontend.Context());auto* window=ImGui::FindWindowByID(ImHashStr("###EditorUi.Hierarchy"));CHECK(window);ImGui::ClearActiveID();ImGui::FocusWindow(window);ImGui::SetCurrentContext(previous);
    }
    input.keyEvents.clear();frontend.Draw(editor,assets,localization,input,viewport,&ui);
    input.keyEvents={{static_cast<ImGuiKey>(ImGuiMod_Ctrl),true},{ImGuiKey_Z,true}};frontend.Draw(editor,assets,localization,input,viewport,&ui);
    CHECK(Near(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(child))->position.y,worldBeforeShortcut.position.y)&&ui.Get(button)->text=="Canvas shortcut"&&!frontend.CurrentMaterialPreview());
    input.keyEvents={{ImGuiKey_Z,false},{static_cast<ImGuiKey>(ImGuiMod_Ctrl),false}};frontend.Draw(editor,assets,localization,input,viewport,&ui);
    // 面板显隐必须影响实际窗口，而不只是保存一个布尔值。
    const auto savedLayout=directory/"editor-layout.json";
    se::EditorLayout layout;layout.SetVisible(se::EditorPanel::Assets,false);layout.SetVisible(se::EditorPanel::UiDesigner,false);
    std::string layoutError;CHECK(layout.Save(savedLayout,layoutError));
    for(const auto size:{ImVec2{1280,720},ImVec2{1920,1080}})for(const auto scale:{1.0f,1.5f,2.0f})
    {
        se::ImGuiEditor restored;restored.SetLayoutPath(savedLayout);
        const unsigned char* restoredPixels=nullptr;int restoredWidth=0,restoredHeight=0;
        restored.FontAtlasRgba(restoredPixels,restoredWidth,restoredHeight);restored.SetFontTexture(1);
        restored.SetUiScale(1,scale);
        se::EditorInputFrame frame;frame.displaySize=size;
        for(int pass=0;pass<3;++pass)restored.Draw(editor,assets,localization,frame,viewport,nullptr);
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(restored.Context());
        const auto* hidden=ImGui::FindWindowByID(ImHashStr("###EditorUi.Assets"));CHECK(!hidden||!hidden->Active);
        const auto* hierarchy=ImGui::FindWindowByID(ImHashStr("###EditorUi.Hierarchy"));CHECK(hierarchy&&hierarchy->Active);
        const auto* sceneWindow=ImGui::FindWindowByID(ImHashStr("###EditorUi.Viewport"));
        const auto* inspectorWindow=ImGui::FindWindowByID(ImHashStr("###EditorUi.Inspector"));
        const auto* consoleWindow=ImGui::FindWindowByID(ImHashStr("###EditorUi.Console"));
        const auto* toolbar=ImGui::FindWindowByID(ImHashStr("###EditorUi.Toolbar"));
        CHECK(sceneWindow&&consoleWindow&&toolbar&&inspectorWindow);
        CHECK(sceneWindow->DockIsActive&&sceneWindow->DockNode&&hierarchy->DockIsActive&&consoleWindow->DockIsActive);
        CHECK(sceneWindow->Pos.y>=toolbar->Pos.y+toolbar->Size.y-1);
        CHECK(hierarchy->Pos.x+hierarchy->Size.x<=sceneWindow->Pos.x+1);
        CHECK(sceneWindow->Pos.x+sceneWindow->Size.x<=inspectorWindow->Pos.x+1);
        CHECK(inspectorWindow->Pos.y+inspectorWindow->Size.y<=consoleWindow->Pos.y+1);
        CHECK(sceneWindow->Pos.x>=0&&sceneWindow->Pos.y>=0&&sceneWindow->Pos.x+sceneWindow->Size.x<=size.x+1
            &&sceneWindow->Pos.y+sceneWindow->Size.y<=size.y+1);
        ImGui::SetCurrentContext(previous);
    }
    // 托管快照不可用时，不能把未知绑定状态当作干净文档直接保存成功或关闭。
    {
        se::EditorSession clean;const auto scene=directory/"external-failure.scene.json";
        CHECK(clean.Save(scene));
        se::ImGuiEditor guarded;guarded.SetProjectRoot(directory);guarded.SetDocumentPaths(scene,{});
        const unsigned char* guardedPixels=nullptr;int guardedWidth=0,guardedHeight=0;
        guarded.FontAtlasRgba(guardedPixels,guardedWidth,guardedHeight);guarded.SetFontTexture(1);
        bool available=false;se::EditorExternalDocuments external;
        external.snapshot=[&](std::vector<se::EditorExternalDocument>& snapshots,std::string& error){snapshots.clear();error.clear();return available;};
        guarded.SetExternalDocuments(std::move(external));se::EditorInputFrame frame;
        guarded.Draw(clean,assets,localization,frame,viewport,nullptr);
        CHECK(guarded.ExecuteCommand("editor.save-all").state==se::EditorCommandResult::Failed);
        guarded.RequestClose();guarded.Draw(clean,assets,localization,frame,viewport,nullptr);
        CHECK(!guarded.CloseApproved()&&guarded.FileDialogActive());
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(guarded.Context());
        auto* modal=ImGui::FindWindowByID(ImHashStr("###EditorUi.UnsavedDocuments"));CHECK(modal);
        ImGui::ActivateItemByID(modal->GetID("###EditorUi.SaveAndClose"));ImGui::SetCurrentContext(previous);
        guarded.Draw(clean,assets,localization,frame,viewport,nullptr);
        CHECK(!guarded.CloseApproved()&&guarded.FileDialogActive());
        ImGui::SetCurrentContext(guarded.Context());ImGui::ActivateItemByID(modal->GetID("###EditorUi.Cancel"));ImGui::SetCurrentContext(previous);
        guarded.Draw(clean,assets,localization,frame,viewport,nullptr);CHECK(!guarded.FileDialogActive());
        available=true;guarded.Draw(clean,assets,localization,frame,viewport,nullptr);
        CHECK(guarded.ExecuteCommand("editor.save-all").state==se::EditorCommandResult::Succeeded);
        guarded.RequestClose();guarded.Draw(clean,assets,localization,frame,viewport,nullptr);CHECK(guarded.CloseApproved());
    }
    std::cout<<"ImGuiEditorTests passed (CPU draw data, font atlas, transform gizmos, undo, UI persistence, actual panel visibility and restored geometry, unavailable external document protection).\n";
    return 0;
}
