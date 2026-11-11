#include "ImGuiEditor.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <imgui/imgui_internal.h>
#include <chrono>
#include <algorithm>
#include <cstdio>

namespace se
{
namespace
{
std::string Digest(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    return !path.empty()&&asset_io::Read(path,bytes)?asset_io::Hash(bytes):std::string{};
}
std::string RelativeDocument(const std::filesystem::path& root,const std::filesystem::path& path)
{
    if(path.empty())return {};
    std::error_code error;const auto absolute=std::filesystem::weakly_canonical(path,error);
    if(error||!asset_io::Within(root,absolute))return {};
    return asset_io::Utf8(absolute.lexically_relative(root));
}
template<std::size_t N> void SetPath(std::array<char,N>& field,const std::filesystem::path& path)
{const auto text=asset_io::Utf8(path);std::snprintf(field.data(),field.size(),"%s",text.c_str());}
bool SameDocument(const std::filesystem::path& left,const std::filesystem::path& right)
{return !left.empty()&&!right.empty()&&asset_io::Within(left,right)&&asset_io::Within(right,left);}
}

void ImGuiEditor::ConfigureRecovery(std::filesystem::path directory,bool offerRecovery)
{
    recoveryDirectory_=std::move(directory);recoveryGroups_.clear();recoveryProtectedGroups_.clear();recoveryPrunedSignature_.clear();
    if(offerRecovery)
    {
        std::string error;recoveryGroups_=EditorSessionStore::ListRecoveryGroups(recoveryDirectory_,projectRoot_,error);
        if(!error.empty())LogConsole(1,error);
    }
    recoveryOffered_=!recoveryGroups_.empty();
}

std::vector<std::string> ImGuiEditor::SessionDocuments(const EditorSession& editor) const
{
    std::vector<std::string> result;
    for(const auto& path:{editor.Path(),activeMaterialPath_,uiSavedPath_})
    {const auto relative=RelativeDocument(projectRoot_,path);if(!relative.empty())result.push_back(relative);}
    for(const auto* document:{&projectSettingsDocument_,&buildSettingsDocument_})if(document->Loaded())
    {
        const auto relative=RelativeDocument(projectRoot_,document->Path());
        if(!relative.empty()&&std::find(result.begin(),result.end(),relative)==result.end())result.push_back(relative);
    }
    return result;
}

std::string ImGuiEditor::SessionActiveDocument(const EditorSession& editor) const
{
    std::filesystem::path path;
    switch(documentContext_)
    {
    case DocumentContext::Material:if(materialReady_)path=activeMaterialPath_;break;
    case DocumentContext::Ui:if(uiOwner_)path=uiSavedPath_;break;
    case DocumentContext::ProjectSettings:if(projectSettingsDocument_.Loaded())path=projectSettingsDocument_.Path();break;
    case DocumentContext::BuildSettings:if(buildSettingsDocument_.Loaded())path=buildSettingsDocument_.Path();break;
    case DocumentContext::Scene:path=editor.Path();break;
    }
    const auto relative=RelativeDocument(projectRoot_,path);
    return relative.empty()?RelativeDocument(projectRoot_,editor.Path()):relative;
}

void ImGuiEditor::RestoreSessionDocuments(const std::vector<std::string>& documents,AssetStore& assets,UiTree& ui,std::string_view activePath)
{
    pendingSessionFocus_.reset();pendingSessionDocument_.clear();
    const auto previousContext=documentContext_;
    auto active=asset_io::FromUtf8(activePath);
    if(!active.empty()&&!active.is_absolute())active=projectRoot_/active;
    if(!active.empty()&&!asset_io::Within(projectRoot_,active))active.clear();
    for(const auto& document:documents)
    {
        const auto requested=asset_io::FromUtf8(document);
        const auto path=requested.is_absolute()?requested:projectRoot_/requested;
        if(!asset_io::Within(projectRoot_,path)){LogConsole(1,"Editor.SessionDocumentMissing",document);continue;}
        std::optional<EditorPanel> restored;
        if(document.ends_with(".material.json"))
        {if(OpenMaterial(assets,path)){showMaterial_=true;restored=EditorPanel::MaterialEditor;}else LogConsole(1,errorKey_,document);}
        else if(document.ends_with(".ui.json"))
        {if(OpenUi(ui,path,true)){SetPath(uiPath_,path);showUi_=true;restored=EditorPanel::UiDesigner;}else LogConsole(1,errorKey_,document);}
        else if(SameDocument(path,projectRoot_/"project.json"))
        {
            LoadProjectSettings();
            if(projectSettingsError_.empty()){showProjectSettings_=true;restored=EditorPanel::ProjectSettings;}
            else LogConsole(1,projectSettingsError_,document);
        }
        else if(SameDocument(path,projectRoot_/"build.json"))
        {
            LoadBuildSettings();
            if(buildSettingsDocument_.Loaded()&&(buildSettingsError_.empty()||buildSettingsError_=="Project.BuildSettingsMissing"))
            {showBuild_=true;restored=EditorPanel::BuildExport;}
            else LogConsole(1,buildSettingsError_,document);
        }
        else if(document.ends_with(".scene.json"))
        {
            // 场景由应用入口打开；首次 Draw 还会校验它确实是当前 EditorSession 的场景。
            std::error_code error;if(std::filesystem::is_regular_file(path,error)&&!error)restored=EditorPanel::Viewport;
        }
        if(restored&&SameDocument(path,active)){pendingSessionFocus_=restored;pendingSessionDocument_=path;}
    }
    // 打开材质的副作用不能决定最终活动文档；旧会话没有 active 时保留原上下文。
    documentContext_=previousContext;
    if(!pendingSessionFocus_)return;
    switch(*pendingSessionFocus_)
    {
    case EditorPanel::MaterialEditor:documentContext_=DocumentContext::Material;break;
    case EditorPanel::UiDesigner:documentContext_=DocumentContext::Ui;break;
    case EditorPanel::ProjectSettings:documentContext_=DocumentContext::ProjectSettings;break;
    case EditorPanel::BuildExport:documentContext_=DocumentContext::BuildSettings;break;
    case EditorPanel::Viewport:documentContext_=DocumentContext::Scene;showViewport_=true;break;
    default:break;
    }
}

void ImGuiEditor::ApplySessionDocumentFocus(const EditorSession& editor)
{
    if(!pendingSessionFocus_)return;
    std::filesystem::path path;const char* windowName=nullptr;auto context=DocumentContext::Scene;
    switch(*pendingSessionFocus_)
    {
    case EditorPanel::MaterialEditor:path=activeMaterialPath_;windowName="###EditorUi.MaterialEditor";context=DocumentContext::Material;break;
    case EditorPanel::UiDesigner:path=uiSavedPath_;windowName="###EditorUi.UiDesigner";context=DocumentContext::Ui;break;
    case EditorPanel::ProjectSettings:path=projectSettingsDocument_.Path();windowName="###EditorUi.ProjectSettings";context=DocumentContext::ProjectSettings;break;
    case EditorPanel::BuildExport:path=buildSettingsDocument_.Path();windowName="###EditorUi.BuildExport";context=DocumentContext::BuildSettings;break;
    case EditorPanel::Viewport:path=editor.Path();windowName="###EditorUi.Viewport";break;
    default:break;
    }
    if(!windowName||!SameDocument(path,pendingSessionDocument_))
    {pendingSessionFocus_.reset();pendingSessionDocument_.clear();return;}
    if(FileDialogActive()||ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))return;
    const auto window=ImGui::FindWindowByID(ImHashStr(windowName));
    if(!window||!window->Active||window->Hidden)return;
    ImGui::FocusWindow(window);documentContext_=context;
    pendingSessionFocus_.reset();pendingSessionDocument_.clear();
}

std::vector<RecoveryDocument> ImGuiEditor::CaptureRecoveryDocuments(const EditorSession& editor,UiTree* ui)
{
    recoveryCaptureValid_=false;
    std::vector<RecoveryDocument> documents;
    const auto add=[&](std::string id,std::string kind,const std::filesystem::path& path,std::string base,std::string content){
        documents.push_back({std::move(id),std::move(kind),RelativeDocument(projectRoot_,path),std::move(base),std::move(content),0});
    };
    if(editor.Dirty())
    {
        std::string scene,error;if(SerializeScene(editor.EditWorld(),scene,error))add("scene","scene",editor.Path(),editor.SourceDigest(),std::move(scene));
        else{LogConsole(1,error);return {};}
    }
    if(MaterialDirty())
    {
        std::string material;std::vector<AssetDiagnostic> diagnostics;
        if(!SerializeMaterialDocument(material_.Document(),material,diagnostics))
        {LogConsole(1,"Assets.InvalidMaterial");return {};}
        add("material","material",activeMaterialPath_,materialStamp_.digest,std::move(material));
    }
    if(UiDirty(ui))add("ui","ui",uiSavedPath_,uiStamp_.digest,ui->SaveJson());
    SyncSettingsDrafts();
    for(const auto* settings:{&projectSettingsDocument_,&buildSettingsDocument_})if(settings->Dirty())
    {RecoveryDocument document;std::string error;if(!settings->Snapshot(document,error)){LogConsole(1,error);return {};}documents.push_back(std::move(document));}
    if(externalDocuments_.snapshot)
    {
        std::string error;std::vector<EditorExternalDocument> snapshots;
        if(externalDocuments_.snapshot(snapshots,error))
        {externalDocumentSnapshots_=std::move(snapshots);for(const auto& external:externalDocumentSnapshots_)if(external.dirty)documents.push_back(external.document);}
        else{LogConsole(1,error);return {};}
    }
    std::string signature;
    for(const auto& document:documents)signature+=document.documentId+document.relativePath+document.sourceDigest+document.content;
    const auto digest=asset_io::Hash({reinterpret_cast<const std::uint8_t*>(signature.data()),signature.size()});
    if(digest!=recoverySignature_){recoverySignature_=digest;++recoveryRevision_;}
    for(auto& document:documents)document.revision=recoveryRevision_;
    recoveryCaptureValid_=true;
    return documents;
}

bool ImGuiEditor::RecoverySourceChanged(const RecoveryGroup& group) const
{
    for(const auto& document:group.documents)
    {
        if(document.relativePath.empty())continue;
        const auto path=projectRoot_/asset_io::FromUtf8(document.relativePath);
        if(!asset_io::Within(projectRoot_,path)||Digest(path)!=document.sourceDigest)return true;
    }
    return false;
}

bool ImGuiEditor::RestoreRecovery(const RecoveryGroup& group,EditorSession& editor,AssetStore& assets,UiTree& ui)
{
    if(editor.IsPlaying()||editor.Dirty()||DocumentsDirty(&ui))
    {errorKey_="Editor.UnsavedChanges";return false;}
    if(RecoverySourceChanged(group)){errorKey_="Editor.RecoverySourceChanged";return false;}
    const RecoveryDocument* scene=nullptr,*material=nullptr,*uiDocument=nullptr;
    std::vector<const RecoveryDocument*> settingsDocuments;
    std::vector<const RecoveryDocument*> external;
    MaterialDocument parsedMaterial;UiTree parsedUi;World parsedScene;
    for(const auto& document:group.documents)
    {
        if(document.kind=="scene"&&!scene)
        {if(!DeserializeScene(document.content,parsedScene,errorKey_))return false;scene=&document;}
        else if(document.kind=="material"&&!material)
        {
            std::vector<AssetDiagnostic> diagnostics;
            if(!DeserializeMaterialDocument(document.content,parsedMaterial,diagnostics)){errorKey_="Assets.InvalidMaterial";return false;}
            if(!document.relativePath.empty()&&!asset_io::Within(assets.SourceRoot(),projectRoot_/asset_io::FromUtf8(document.relativePath)))
            {errorKey_="Assets.ReferenceOutsideRoot";return false;}
            material=&document;
        }
        else if(document.kind=="ui"&&!uiDocument)
        {if(!parsedUi.LoadJson(document.content,errorKey_))return false;uiDocument=&document;}
        else if(document.kind=="project-settings"||document.kind=="build-settings")
        {
            const bool project=document.kind=="project-settings";
            if(project&&!projectSettingsDocument_.Loaded())LoadProjectSettings();
            if(!project&&!buildSettingsDocument_.Loaded())LoadBuildSettings();
            auto& settings=project?projectSettingsDocument_:buildSettingsDocument_;
            if(!settings.Restore(document,false,errorKey_))return false;settingsDocuments.push_back(&document);
        }
        else if(externalDocuments_.restore&&externalDocuments_.restore(document,false,errorKey_))external.push_back(&document);
        else{if(errorKey_.empty())errorKey_="Editor.RecoveryInvalidDocument";return false;}
    }
    const auto path=[&](const RecoveryDocument& document){return document.relativePath.empty()?std::filesystem::path{}:projectRoot_/asset_io::FromUtf8(document.relativePath);};
    if(scene&&!editor.RestoreUnsavedDocument(scene->content,path(*scene),scene->sourceDigest)){errorKey_=editor.LastError();return false;}
    if(scene){recoveryScenePath_=path(*scene);recoverySceneDigest_=scene->sourceDigest;if(!editor.Path().empty()){SetPath(scenePath_,editor.Path());SetPath(sceneSavePath_,editor.Path());}}
    if(material)
    {
        material_.Reset(parsedMaterial,false);materialReady_=true;materialField_.clear();activeMaterialPath_=path(*material);activeMaterialId_.clear();
        for(const auto& id:assets.ListIds())if(assets.Resolve(id)==activeMaterialPath_){activeMaterialId_=id;break;}
        SetPath(materialPath_,activeMaterialPath_.empty()?assets.SourceRoot()/"Materials"/("Recovered-"+asset_io::NewId()+".material.json"):activeMaterialPath_);
        RecordStamp(materialStamp_,activeMaterialPath_);showMaterial_=true;
    }
    if(uiDocument)
    {
        BindUi(ui);ui=std::move(parsedUi);uiSavedPath_=path(*uiDocument);uiSaved_.clear();
        SetPath(uiPath_,uiSavedPath_.empty()?assets.SourceRoot()/"UI"/("Recovered-"+asset_io::NewId()+".ui.json"):uiSavedPath_);
        RecordStamp(uiStamp_,uiSavedPath_);uiUndo_.clear();uiRedo_.clear();selectedUi_=0;showUi_=true;
    }
    for(const auto* document:external)if(!externalDocuments_.restore(*document,true,errorKey_))return false;
    for(const auto* document:settingsDocuments)
    {
        const bool project=document->kind=="project-settings";auto& settings=project?projectSettingsDocument_:buildSettingsDocument_;
        if(!settings.Restore(*document,true,errorKey_))return false;
        if(project)projectFieldChanges_=0;else buildFieldChanges_=0;
        const auto copy=[](auto& field,const std::string& value){std::snprintf(field.data(),field.size(),"%s",value.c_str());};
        if(project)
        {projectSettings_=settings.Project();copy(projectNameField_,projectSettings_.name);copy(contentRootField_,projectSettings_.contentRoot);copy(entrySceneField_,projectSettings_.entryScene);showProjectSettings_=true;}
        else
        {buildSettings_=settings.Build();copy(scriptsProjectField_,buildSettings_.scriptsProject);copy(scriptTypeField_,buildSettings_.scriptType);copy(bindingsField_,buildSettings_.bindings);copy(entryUiField_,buildSettings_.entryUi);showBuild_=true;}
    }
    errorKey_.clear();return true;
}

bool ImGuiEditor::SaveRecoveryCopy(const RecoveryGroup& group,std::filesystem::path& destination)
{
    destination=projectRoot_/"Recovered"/(group.id+"-"+asset_io::NewId());
    std::error_code error;if(!std::filesystem::create_directories(destination,error)||error){errorKey_="Editor.RecoveryWriteFailed";return false;}
    for(const auto& document:group.documents)
    {
        const auto relative=document.relativePath.empty()?asset_io::FromUtf8(document.documentId+(document.kind=="scene"?".scene.json":document.kind=="material"?".material.json":document.kind=="bindings"?".bindings.json":".ui.json")):asset_io::FromUtf8(document.relativePath);
        const auto target=destination/relative;
        if(!asset_io::Within(destination,target)||!asset_io::WriteAtomic(target,{reinterpret_cast<const std::uint8_t*>(document.content.data()),document.content.size()},false))
        {errorKey_="Editor.RecoveryWriteFailed";return false;}
    }
    errorKey_.clear();LogConsole(0,"Editor.RecoveryCopySaved",asset_io::Utf8(destination));return true;
}

void ImGuiEditor::FinishRecovery(bool discardCurrent)
{
    if(recoveryDirectory_.empty()||!discardCurrent)return;
    std::string error;const auto groups=EditorSessionStore::ListRecoveryGroups(recoveryDirectory_,projectRoot_,error);
    for(const auto& group:groups)if(std::find(recoveryProtectedGroups_.begin(),recoveryProtectedGroups_.end(),group.id)==recoveryProtectedGroups_.end())
        if(!EditorSessionStore::DiscardRecoveryGroup(recoveryDirectory_,group.id,error))LogConsole(1,error);
}

void ImGuiEditor::PruneRecoveryDocuments(const std::vector<RecoveryDocument>& current)
{
    if(!recoveryCaptureValid_||recoverySignature_==recoveryPrunedSignature_)return;
    std::string error;const auto groups=EditorSessionStore::ListRecoveryGroups(recoveryDirectory_,projectRoot_,error);
    if(!error.empty()){LogConsole(1,error);return;}
    std::vector<std::string> obsolete;
    for(const auto& group:groups)
    {
        if(std::find(recoveryProtectedGroups_.begin(),recoveryProtectedGroups_.end(),group.id)!=recoveryProtectedGroups_.end())continue;
        bool changed=false;
        for(const auto& document:group.documents)
            if(!std::any_of(current.begin(),current.end(),[&](const auto& active){return active.documentId==document.documentId&&active.relativePath==document.relativePath&&active.sourceDigest==document.sourceDigest;}))changed=true;
        if(changed)obsolete.push_back(group.id);
    }
    // 始终发布当前完整编辑集合，不能重新发布历史内容让旧修订变成“最新”。
    if(!obsolete.empty()&&!current.empty())
    {
        if(!EditorSessionStore::WriteRecoveryGroup(recoveryDirectory_,projectRoot_,current,error,200ull*1024*1024,5,recoveryProtectedGroups_)){LogConsole(1,error);return;}
        const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        recoveryDebounce_.Mark(now,recoveryRevision_);
    }
    for(const auto& id:obsolete)
        if(!EditorSessionStore::DiscardRecoveryGroup(recoveryDirectory_,id,error)&&error!="Editor.RecoveryNotFound"){LogConsole(1,error);return;}
    recoveryPrunedSignature_=recoverySignature_;
}

void ImGuiEditor::RecoveryDialogs(EditorSession& editor,AssetStore& assets,UiTree* ui,const Localization& text)
{
    if(recoveryDirectory_.empty())return;
    if(recoveryOffered_&&!recoveryGroups_.empty())ImGui::OpenPopup("editor-recovery");
    if(ImGui::BeginPopupModal("editor-recovery",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        if(!recoveryGroups_.empty())
        {
            const auto& group=recoveryGroups_.front();const bool changed=RecoverySourceChanged(group);
            ImGui::TextUnformatted(text.GetText("Editor.RecoveryPrompt").c_str());ImGui::TextUnformatted(group.createdUtc.c_str());
            for(const auto& document:group.documents)ImGui::BulletText("%s: %s",document.kind.c_str(),document.relativePath.c_str());
            if(changed)ImGui::TextWrapped("%s",text.GetText("Editor.RecoverySourceChanged").c_str());
            ImGui::BeginDisabled(changed||!ui);
            if(ImGui::Button(Label(text,"Editor.RecoveryRestore").c_str())&&RestoreRecovery(group,editor,assets,*ui))
            {recoveryOffered_=false;recoveryGroups_.clear();ImGui::CloseCurrentPopup();}
            ImGui::EndDisabled();ImGui::SameLine();
            if(ImGui::Button(Label(text,"Editor.RecoverySaveCopy").c_str()))
            {std::filesystem::path destination;if(SaveRecoveryCopy(group,destination)){for(const auto& existing:recoveryGroups_)recoveryProtectedGroups_.push_back(existing.id);recoveryOffered_=false;ImGui::CloseCurrentPopup();}}
            ImGui::SameLine();if(ImGui::Button(Label(text,"Editor.RecoveryDiscard").c_str()))
            {FinishRecovery(true);recoveryOffered_=false;recoveryGroups_.clear();ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(Label(text,"Editor.RecoveryDefer").c_str()))
            {for(const auto& existing:recoveryGroups_)recoveryProtectedGroups_.push_back(existing.id);recoveryOffered_=false;ImGui::CloseCurrentPopup();}
        }
        if(!errorKey_.empty())ImGui::TextWrapped("%s",text.GetText(errorKey_).c_str());
        ImGui::EndPopup();
    }
    if(recoveryOffered_||editor.IsPlaying()||gizmo_.Active()||inspectorEditing_||material_.Editing()||!uiTransaction_.empty())return;
    const auto documents=CaptureRecoveryDocuments(editor,ui);
    PruneRecoveryDocuments(documents);
    const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(!documents.empty()&&now>=recoveryRetryAfter_&&recoveryDebounce_.Due(now,recoveryRevision_))
    {
        std::string error;
        if(EditorSessionStore::WriteRecoveryGroup(recoveryDirectory_,projectRoot_,documents,error,200ull*1024*1024,5,recoveryProtectedGroups_)){recoveryDebounce_.Mark(now,recoveryRevision_);recoveryRetryAfter_=0;}
        else{LogConsole(1,error);recoveryRetryAfter_=now+60;}
    }
}
}
