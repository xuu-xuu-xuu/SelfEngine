#include "ImGuiEditor.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace se
{
void ImGuiEditor::ResolvePendingReference(EditorSession& editor)
{
    if(!pendingAssetReference_)return;
    const auto reference=*pendingAssetReference_;
    const auto same=[](const auto& left,const auto& right){return !left.empty()&&!right.empty()&&asset_io::Within(left,right)&&asset_io::Within(right,left);};
    if(reference.kind==EditorAssetDocumentKind::Ui)
    {
        if(!pendingUiOpen_.empty())return;
        if(uiOwner_&&(reference.document.empty()||same(uiSavedPath_,reference.document))&&uiOwner_->Get(reference.entity))
        {selectedUi_=reference.entity;showUi_=true;uiCanvasMode_=true;documentContext_=DocumentContext::Ui;}
    }
    else if(reference.kind==EditorAssetDocumentKind::Scene)
    {
        if(files_.Pending())return;
        if((reference.document.empty()||same(editor.Path(),reference.document))&&editor.EditWorld().FindByStableId(reference.entity))editor.Select(reference.entity);
    }
    pendingAssetReference_.reset();
}
void ImGuiEditor::LocateAssetReference(EditorSession& editor,AssetStore& assets,const EditorAssetReference& reference)
{
    if(!reference.document.empty()&&!asset_io::Within(projectRoot_,reference.document)){errorKey_="Assets.ReferenceOutsideRoot";return;}
    const auto same=[](const auto& left,const auto& right){return !left.empty()&&!right.empty()&&asset_io::Within(left,right)&&asset_io::Within(right,left);};
    if(reference.kind==EditorAssetDocumentKind::Ui)
    {
        if(!uiOwner_)return;
        if(reference.inMemory&&(reference.document.empty()||same(uiSavedPath_,reference.document)||same(asset_io::FromUtf8(uiPath_.data()),reference.document)))
        {if(uiOwner_->Get(reference.entity)){selectedUi_=reference.entity;showUi_=true;uiCanvasMode_=true;documentContext_=DocumentContext::Ui;}return;}
        if(UiDirty(uiOwner_)){pendingUiOpen_=reference.document;pendingAssetReference_=reference;return;}
        if(OpenUi(*uiOwner_,reference.document,true)){std::snprintf(uiPath_.data(),uiPath_.size(),"%s",asset_io::Utf8(reference.document).c_str());pendingAssetReference_=reference;ResolvePendingReference(editor);}
    }
    else if(reference.kind==EditorAssetDocumentKind::Scene)
    {
        if((reference.inMemory&&reference.document.empty())||same(editor.Path(),reference.document))
        {if(editor.EditWorld().FindByStableId(reference.entity))editor.Select(reference.entity);return;}
        const auto result=files_.RequestOpen(editor,reference.document);
        if(result==FileActionResult::Failed)errorKey_=files_.LastError();else{pendingAssetReference_=reference;ResolvePendingReference(editor);}
    }
    else if(reference.kind==EditorAssetDocumentKind::Material)
    {showMaterial_=true;OpenMaterial(assets,reference.document);}
    else if(!reference.document.empty())OpenCodeSource(reference.document);
}
void ImGuiEditor::SyncAssetDeclarations(AssetStore& assets)
{
    if(projectRoot_.empty()||!std::filesystem::is_regular_file(projectRoot_/"project.json"))return;
    std::vector<EditorAssetIssue> issues;
    if(!EditorAssetActions::SyncProjectAssets(projectRoot_,assets,issues))
        for(const auto& issue:issues){errorKey_=issue.key;LogConsole(2,issue.key,issue.detail);}
}
bool ImGuiEditor::CaptureEditingDocuments(const EditorSession& editor,AssetStore& assets,UiTree* ui,std::vector<EditorAssetDocument>& documents)
{
    documents.clear();std::string scene,error;
    if(!SerializeScene(editor.EditWorld(),scene,error)){errorKey_=error;return false;}
    documents.push_back({EditorAssetDocumentKind::Scene,editor.Path().empty()?asset_io::FromUtf8(sceneSavePath_.data()):editor.Path(),std::move(scene)});
    if(materialReady_)
    {
        std::string material;std::vector<AssetDiagnostic> diagnostics;
        const auto target=activeMaterialPath_.empty()?asset_io::FromUtf8(materialPath_.data()):activeMaterialPath_;
        if(!SerializeMaterialDocument(material_.Document(),material,diagnostics)){errorKey_=diagnostics.empty()?"Assets.InvalidMaterial":diagnostics.back().key;return false;}
        documents.push_back({EditorAssetDocumentKind::Material,target.is_absolute()?target:assets.SourceRoot()/target,std::move(material)});
    }
    if(ui)documents.push_back({EditorAssetDocumentKind::Ui,uiSavedPath_.empty()?asset_io::FromUtf8(uiPath_.data()):uiSavedPath_,ui->SaveJson()});
    if(externalDocuments_.snapshot)
    {
        std::vector<EditorExternalDocument> snapshots;
        if(!externalDocuments_.snapshot(snapshots,error)){errorKey_=error.empty()?"Editor.DocumentSnapshotFailed":error;return false;}
        for(const auto& snapshot:snapshots)if(snapshot.document.kind=="bindings")documents.push_back({EditorAssetDocumentKind::Bindings,projectRoot_/asset_io::FromUtf8(snapshot.document.relativePath),snapshot.document.content});
    }
    return true;
}
void ImGuiEditor::OpenCodeSource(const std::filesystem::path& source,std::uint32_t line,std::uint32_t column)
{
    std::string key,detail;CodeEditorSettings settings;settings.executable=asset_io::FromUtf8(preferences_.externalEditor);
    if(!EditorScriptTools::OpenSource(projectRoot_,source,line,column,settings,key,detail)){errorKey_=key;LogConsole(2,key,detail);}
}
void ImGuiEditor::AssetActionsToolbar(EditorSession& editor,AssetStore& assets,const Localization& text)
{
    ImGui::SetNextItemWidth(160);ImGui::InputText(Label(text,"Editor.ScriptClassName").c_str(),scriptClassName_.data(),scriptClassName_.size());
    ImGui::SameLine();if(ImGui::Button(Label(text,"Editor.CreateBehaviour").c_str()))
    {
        SyncSettingsDrafts();if(buildSettingsDocument_.Dirty()&&!SaveSettings(false))return;
        EditorScriptCreateResult result;std::string key,detail;
        if(EditorScriptTools::CreateBehaviour({projectRoot_,scriptClassName_.data(),"Game"},result,key,detail))
        {buildSettingsLoaded_=false;LogConsole(0,"Editor.ScriptCreated",asset_io::Utf8(result.source));OpenCodeSource(result.source);}
        else{errorKey_=key;LogConsole(2,key,detail);}
    }
    if(selectedAsset_.empty())return;
    ImGui::SetNextItemWidth(350);ImGui::InputText(Label(text,"Editor.AssetTarget").c_str(),assetTarget_.data(),assetTarget_.size());
    const auto plan=[&](EditorAssetAction action){
        EditorAssetActionRequest request;request.action=action;request.projectRoot=projectRoot_;request.contentRoot=assets.SourceRoot();
        request.source=assets.Resolve(selectedAsset_);request.target=assets.SourceRoot()/asset_io::FromUtf8(assetTarget_.data());
        if(MaterialDirty()&&!activeMaterialPath_.empty()&&asset_io::Within(activeMaterialPath_,request.source)&&asset_io::Within(request.source,activeMaterialPath_))
        {errorKey_="Editor.AssetDocumentDirty";return;}
        if(!CaptureEditingDocuments(editor,assets,uiOwner_,request.documents)){LogConsole(2,"Editor.DocumentSnapshotFailed",errorKey_);return;}
        AssetStore snapshot=assets;
        assetActionPlanning_=std::async(std::launch::async,[request,snapshot=std::move(snapshot)]{return EditorAssetActions::Plan(request,snapshot);});
    };
    ImGui::SameLine();if(ImGui::Button(Label(text,"Editor.AssetMove").c_str()))plan(EditorAssetAction::Move);
    ImGui::SameLine();if(ImGui::Button(Label(text,"Editor.AssetDelete").c_str()))plan(EditorAssetAction::Delete);
}
void ImGuiEditor::ProjectFiles(EditorSession& editor,AssetStore&,const Localization& text)
{
    ImGui::TextWrapped("%s",assetFolder_.empty()?text.GetText("Editor.ProjectFiles").c_str():asset_io::Utf8(assetFolder_).c_str());
    if(!assetFolder_.empty()&&ImGui::Button(Label(text,"Editor.AssetParent").c_str()))assetFolder_=assetFolder_.parent_path();
    std::error_code error;std::vector<std::filesystem::directory_entry> entries;
    for(std::filesystem::directory_iterator it(projectRoot_/assetFolder_,error),end;!error&&it!=end;it.increment(error))
    {
        const auto name=asset_io::Utf8(it->path().filename());
        if(name=="out"||name=="bin"||name=="obj"||name==".git"||name==".cache"||it->is_symlink(error)||!asset_io::Within(projectRoot_,it->path()))continue;
        if(it->is_directory(error)||name.ends_with(".scene.json")||name.ends_with(".ui.json")||name.ends_with(".cs"))entries.push_back(*it);
    }
    std::sort(entries.begin(),entries.end(),[](const auto& left,const auto& right){return left.path()<right.path();});
    for(const auto& entry:entries)
    {
        const auto path=entry.path();const auto name=asset_io::Utf8(path.filename());const bool directory=entry.is_directory(error);
        if(ImGui::Selectable(((directory?"[+] ":"    ")+name).c_str(),false,ImGuiSelectableFlags_AllowDoubleClick)&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            if(directory)assetFolder_=path.lexically_relative(projectRoot_);
            else if(name.ends_with(".cs"))OpenCodeSource(path);
            else if(name.ends_with(".scene.json"))Queue([this,&editor,path]{if(files_.RequestOpen(editor,path)==FileActionResult::Failed)errorKey_=files_.LastError();});
            else if(uiOwner_)
            {
                if(UiDirty(uiOwner_))pendingUiOpen_=path;
                else if(OpenUi(*uiOwner_,path,true)){std::snprintf(uiPath_.data(),uiPath_.size(),"%s",asset_io::Utf8(path).c_str());showUi_=true;}
            }
        }
    }
}
void ImGuiEditor::AssetActionDialogs(EditorSession& editor,AssetStore& assets,const Localization& text)
{
    using namespace std::chrono_literals;
    if(assetActionPlanning_.valid()&&assetActionPlanning_.wait_for(0s)==std::future_status::ready)
    {try{assetActionCandidate_=assetActionPlanning_.get();}catch(const std::exception& error){LogConsole(2,"Assets.ActionPlanInvalid",error.what());}}
    if(assetActionExecution_.valid()&&assetActionExecution_.wait_for(0s)==std::future_status::ready)
    {
        try
        {
            const auto result=assetActionExecution_.get();assetDiagnostics_.clear();assets.Scan(assetDiagnostics_);
            if(result.success)
            {
                if(materialReady_&&!activeMaterialPath_.empty()&&asset_io::Within(activeMaterialPath_,result.source)&&asset_io::Within(result.source,activeMaterialPath_))
                {
                    if(result.action==EditorAssetAction::Move)
                    {activeMaterialPath_=result.target;std::snprintf(materialPath_.data(),materialPath_.size(),"%s",asset_io::Utf8(result.target).c_str());RecordStamp(materialStamp_,result.target);}
                    else{materialReady_=false;activeMaterialId_.clear();activeMaterialPath_.clear();materialPath_.fill(0);materialStamp_={};}
                }
                selectedAsset_.clear();previewModel_.reset();previewImage_.reset();LogConsole(0,"Editor.AssetActionSucceeded");
            }
            else for(const auto& issue:result.diagnostics){errorKey_=issue.key;LogConsole(2,issue.key,issue.detail);}
        }
        catch(const std::exception& error){LogConsole(2,"Assets.ActionWriteFailed",error.what());}
    }
    if(assetActionCandidate_)ImGui::OpenPopup("asset-action-confirm");
    if(ImGui::BeginPopupModal("asset-action-confirm",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        if(assetActionCandidate_)
        {
            const auto& plan=*assetActionCandidate_;
            ImGui::TextUnformatted(text.GetText(plan.request.action==EditorAssetAction::Delete?"Editor.AssetDeleteConfirm":"Editor.AssetMoveConfirm").c_str());
            ImGui::BeginChild("action-files",{650,260},ImGuiChildFlags_Borders);
            for(const auto& key:plan.requirements)ImGui::TextWrapped("%s",text.GetText(key).c_str());
            for(const auto& file:plan.files)ImGui::TextWrapped("%s -> %s",asset_io::Utf8(file.source).c_str(),asset_io::Utf8(file.target).c_str());
            for(const auto& issue:plan.diagnostics)ImGui::TextWrapped("%s: %s",text.GetText(issue.key).c_str(),issue.detail.c_str());
            for(const auto& reference:plan.references)
            {
                ImGui::TextWrapped("%s: %s",asset_io::Utf8(reference.document).c_str(),reference.pointer.c_str());
                ImGui::PushID(asset_io::Utf8(reference.document).c_str());ImGui::PushID(reference.pointer.c_str());
                if(ImGui::SmallButton(Label(text,"Editor.AssetLocateReference").c_str()))
                {Queue([this,&editor,&assets,reference]{assetActionCandidate_.reset();LocateAssetReference(editor,assets,reference);});ImGui::CloseCurrentPopup();}
                ImGui::PopID();ImGui::PopID();
            }
            ImGui::EndChild();ImGui::BeginDisabled(!plan.valid);
            if(ImGui::Button(Label(text,"Editor.AssetConfirm").c_str()))
            {
                const auto candidate=plan;std::vector<EditorAssetDocument> documents;const auto cache=projectRoot_/"out/Cache/Assets";
                const bool dirtyAsset=MaterialDirty()&&!activeMaterialPath_.empty()&&asset_io::Within(activeMaterialPath_,plan.request.source)&&asset_io::Within(plan.request.source,activeMaterialPath_);
                if(dirtyAsset)errorKey_="Editor.AssetDocumentDirty";
                if(!dirtyAsset&&CaptureEditingDocuments(editor,assets,uiOwner_,documents))
                {
                assetActionExecution_=std::async(std::launch::async,[candidate,documents,cache]{
                    AssetActionResult result;result.action=candidate.request.action;result.source=candidate.request.source;result.target=candidate.request.target;
                    AssetStore worker(candidate.request.contentRoot,cache);std::vector<AssetDiagnostic> scan;
                    if(!worker.Scan(scan)){result.diagnostics.push_back({"Assets.ScanFailed",{}, {}});return result;}
                    result.success=EditorAssetActions::Execute(candidate,worker,documents,result.diagnostics);return result;
                });
                assetActionCandidate_.reset();ImGui::CloseCurrentPopup();
                }
                else LogConsole(2,"Editor.DocumentSnapshotFailed",errorKey_);
            }
            ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){assetActionCandidate_.reset();ImGui::CloseCurrentPopup();}
        }
        ImGui::EndPopup();
    }
    if(!pendingUiOpen_.empty())ImGui::OpenPopup("asset-ui-open");
    if(ImGui::BeginPopupModal("asset-ui-open",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(text.GetText("EditorUi.DiscardPrompt").c_str());
        bool open=false;
        if(ImGui::Button(Label(text,"EditorUi.SaveAndOpen").c_str()))open=uiOwner_&&SaveUi(*uiOwner_,asset_io::FromUtf8(uiPath_.data()));
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.DiscardOpen").c_str()))open=true;
        ImGui::SameLine();if(ImGui::Button(Label(text,"EditorUi.Cancel").c_str())){pendingUiOpen_.clear();ImGui::CloseCurrentPopup();}
        if(open&&uiOwner_&&OpenUi(*uiOwner_,pendingUiOpen_,true))
        {std::snprintf(uiPath_.data(),uiPath_.size(),"%s",asset_io::Utf8(pendingUiOpen_).c_str());pendingUiOpen_.clear();showUi_=true;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
}
}
