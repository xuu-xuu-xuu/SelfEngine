#pragma once
#include "TODPreview.h"
#include "EditorSession.h"
#include "MaterialEditSession.h"
#include "SceneFileWorkflow.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "CoreLayer/Localization.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include "UICanvasEditor.h"
#include "EditorLayout.h"
#include "EditorCommand.h"
#include "EditorPlayback.h"
#include "EditorProcessRunner.h"
#include "ExportJob.h"
#include "ProjectSettingsService.h"
#include "EditorSettingsDocuments.h"
#include "AssetImportService.h"
#include "EditorSessionStore.h"
#include "EditorExternalDocuments.h"
#include "EditorAssetActions.h"
#include "EditorScriptTools.h"
#include "FunctionLayer/Rendering/ColorLutFile.h"
#include <imgui/imgui.h>
#include <array>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <vector>

namespace se
{
enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoAxis { X, Y, Z };
enum class GizmoSpace { World,Local };
Vec3 EditorGizmoAxis(const World& world,EntityHandle entity,GizmoAxis axis,GizmoSpace space);
struct EditorRect { float x = 0, y = 0, width = 1, height = 1; };

// 可选空间；有相机时平移用射线与轴最近点，捕获后允许出面板，投影变化取消。
// 无相机的 CPU 调用保留投影单位标定；旋转/缩放使用像素到弧度/比例的灵敏度。
class EditorGizmo
{
public:
    bool Begin(EditorSession& editor, GizmoMode mode, GizmoAxis axis, Vec2 mouse,
        Vec2 screenDirection,float pixelsPerUnit=80,GizmoSpace space=GizmoSpace::World,const CameraSnapshot* camera=nullptr);
    bool Update(EditorSession& editor, Vec2 mouse);
    bool End(EditorSession& editor, bool commit = true);
    bool Active() const noexcept { return active_; }
    bool ProjectionMatches(const std::optional<CameraSnapshot>& camera) const;
    const std::string& LastError() const noexcept{return error_;}
private:
    bool Fail(const char* key){error_=key;return false;}
    bool active_ = false;
    StableEntityId entity_ = 0;
    GizmoMode mode_{}; GizmoAxis axis_{};
    GizmoSpace space_=GizmoSpace::World;
    Transform initial_;
    Mat4 parentInverse_,initialWorld_;
    Vec3 axisDirection_;
    std::optional<CameraSnapshot> camera_;
    double initialAxisParameter_=0;
    std::string error_;
    Vec2 mouse_{}, direction_{};
    float pixelsPerUnit_ = 80;
};

struct EditorInputFrame
{
    ImVec2 displaySize{1280, 720}, mousePosition{-10000, -10000};
    float deltaSeconds = 1.0f / 60.0f, wheelX = 0, wheelY = 0;
    bool focused = true;
    bool usePlatformInput = false; // 平台适配器已提交事件/设置 DisplaySize 与 DeltaTime 时启用。
    std::array<bool, 5> mouseButtons{};
    std::vector<std::pair<ImGuiKey, bool>> keyEvents;
    std::string textUtf8; // 已提交文字；IME 组合/候选窗口由平台适配器负责。
};
struct EditorViewport
{
    bool hasCamera = false;
    Mat4 viewProjection;
    std::optional<CameraSnapshot> camera;
    ImTextureID colorTexture = ImTextureID_Invalid; // 外部渲染器提供，可为空。
    // 接收视口内归一化坐标（左上为 0,0），返回稳定实体 ID；没有命中返回 0。
    std::function<StableEntityId(float, float)> pick;
    // 在本帧 panel 布局完成、绘制手柄和处理拾取之前更新镜头。
    std::function<void(EditorRect,bool,EditorViewport&)> prepare;
};
struct EditorFrameResult
{
    ImDrawData* drawData = nullptr; // 有效期至本 context 下一次 Draw/销毁。
    EditorRect viewport{0,0,0,0};
    bool captureMouse = false, captureKeyboard = false;
    bool uiCanvas=false;
    std::vector<UiVisual> uiPreview;
};

// 无 Win32/GPU backend。调用方传递输入、上传字体并渲染 ImDrawData。
class ImGuiEditor
{
public:
    ImGuiEditor();
    ~ImGuiEditor();
    ImGuiEditor(const ImGuiEditor&) = delete;
    ImGuiEditor& operator=(const ImGuiEditor&) = delete;
    ImGuiContext* Context() const noexcept { return context_; }
    bool LoadFont(const std::filesystem::path& fontFile, float pixelSize = 18);
    void FontAtlasRgba(const unsigned char*& pixels, int& width, int& height);
    void SetFontTexture(ImTextureID texture);
    void SetAuxiliaryPanels(bool visible){showMaterial_=showUi_=visible;}
    void SetUiCanvasEnabled(bool enabled){uiCanvasMode_=enabled;if(enabled)showUi_=true;}
    void SetLayoutPath(std::filesystem::path path);
    UICanvasEditor& UiCanvas()noexcept{return uiCanvas_;}
    void SetDocumentPaths(const std::filesystem::path& scene, const std::filesystem::path& ui);
    void SetProjectRoot(std::filesystem::path root){projectRoot_=root;files_.SetRoot(std::move(root));}
    void RequestClose(){requestClose_=true;}
    bool CloseApproved() const noexcept{return files_.CloseApproved();}
    bool FileDialogActive() const noexcept{return requestClose_||files_.Pending()||saveAsRequested_||saveAsOpen_||materialOverwrite_||externalDocument_!=ExternalDocument::None||settingsReload_!=0||AssetOperationBusy()||importCandidate_.has_value()||assetActionCandidate_.has_value()||!pendingUiOpen_.empty()||recoveryOffered_;}
    void SetUiScale(float userScale,float dpiScale=1.0f);
    void SetDpiScale(float dpiScale){SetUiScale(uiScale_,dpiScale);}
    void SetDockLayoutPath(std::filesystem::path path);
    void QueueAssetImport(std::vector<std::filesystem::path> sources);
    bool AssetOperationBusy() const noexcept{return importPlanning_.valid()||importExecution_.valid()||assetActionPlanning_.valid()||assetActionExecution_.valid();}
    void ConfigureRecovery(std::filesystem::path directory,bool offerRecovery);
    std::vector<RecoveryDocument> CaptureRecoveryDocuments(const EditorSession&,UiTree*);
    bool RestoreRecovery(const RecoveryGroup&,EditorSession&,AssetStore&,UiTree&);
    bool SaveRecoveryCopy(const RecoveryGroup&,std::filesystem::path& destination);
    bool RecoverySourceChanged(const RecoveryGroup&) const;
    void FinishRecovery(bool discardCurrent);
    std::vector<std::string> SessionDocuments(const EditorSession&) const;
    std::string SessionActiveDocument(const EditorSession&) const;
    void RestoreSessionDocuments(const std::vector<std::string>&,AssetStore&,UiTree&,std::string_view activePath={});
    void ConfigureBuildControl();
    bool PublishRestartRequest();
    void SetExternalDocuments(EditorExternalDocuments documents){externalDocuments_=std::move(documents);externalDocumentsValid_=!externalDocuments_.snapshot;}
    EditorCommandOutcome ExecuteCommand(std::string_view id);
    void ConfigurePreferences(const EditorPreferences&,std::filesystem::path directory,std::function<bool(const std::string&)> language);
    const EditorPreferences& Preferences() const noexcept{return preferences_;}
    // G15：把偏好中的相对 .selut 路径解析为项目根下的绝对路径；空表示未启用。
    std::filesystem::path ColorLutPath() const;
    void SetScriptInspector(std::function<void(StableEntityId,bool)> callback) { scriptInspector_=std::move(callback); }
    // 供工具栏/自动化入口共用同一导出路径；返回 false 时 errorKey 给出原因。
    bool StartExport(const std::string& name,std::string& errorKey);
    bool ExportBusy() const;
    bool ExportFinished() const;
    ExportJobSnapshot ExportSnapshot() const;
    // Toolkit 壳不经过 Draw，需要显式驱动导出快照的轮询/Join。
    void PollExportJob(const Localization& text) { PollExport(text); }
    void RequestCancelExport();
    EditorFrameResult Draw(EditorSession& session, AssetStore& assets, const Localization& text,
        const EditorInputFrame& input, const EditorViewport& viewport = {}, UiTree* ui = nullptr);
    bool UiUndo(UiTree& ui);
    bool UiRedo(UiTree& ui);
    bool SaveUi(UiTree& ui, const std::filesystem::path& path);
    bool OpenUi(UiTree& ui, const std::filesystem::path& path, bool discardUnsaved = false);
    void CancelInteractions(EditorSession& session, UiTree* ui = nullptr);
    UiId SelectedUi() const noexcept { return selectedUi_; }
    bool EditUi(UiTree& ui, const std::function<bool(UiTree&)>& operation);
    bool AssignUiImage(UiTree&,AssetStore&,UiId,const AssetId&);
    void LocateAssetReference(EditorSession&,AssetStore&,const EditorAssetReference&);
    std::optional<MaterialPreview> CurrentMaterialPreview() const;
    // Toolkit 壳的材料编辑桥：复用同一 MaterialEditSession / 文件服务，不复制第二套文档。
    bool OpenMaterialForToolkit(AssetStore& assets, const std::filesystem::path& path, bool discard,
                                std::string& errorKey);
    bool SaveMaterialForToolkit(AssetStore& assets, std::string& errorKey);
    bool MaterialReadyForToolkit() const noexcept { return materialReady_; }
    bool MaterialDirtyForToolkit() const { return MaterialDirty(); }
    const MaterialDocument& MaterialForToolkit() const noexcept { return material_.Document(); }
    bool SetMaterialPropertyForToolkit(std::string_view key, const PropertyValue& value);
    // Toolkit UI Designer：复用同一 UICanvasEditor 手势与撤销事务。
    void SelectUiForToolkit(UiId id) { selectedUi_ = id; }
    bool BeginUiCanvasDragForToolkit(UiTree& ui, UiId id, UiPoint screen);
    bool UpdateUiCanvasDragForToolkit(UiTree& ui, UiPoint screen);
    bool EndUiCanvasDragForToolkit(UiTree& ui, bool commit);
    TODPreview& TimeOfDayPreview()noexcept{return todPreview_;}

private:
    struct ConsoleEntry{int level=0;std::string key,detail;};
    struct FileStamp{std::uint64_t size=0;std::filesystem::file_time_type time{};bool valid=false;std::string digest;};
    enum class ExternalDocument{None,Material,Ui,ProjectSettings,BuildSettings};
    std::function<void(StableEntityId,bool)> scriptInspector_;
    std::string Label(const Localization& text, const char* key) const;
    void Menu(EditorSession&,AssetStore&,const Localization&,UiTree*);
    void FileDialogs(EditorSession&,AssetStore&,const Localization&);
    void Hierarchy(EditorSession&, const Localization&);
    void Inspector(EditorSession&, AssetStore&, const Localization&);
    void ComponentFields(EditorSession&,StableEntityId,std::span<const PropertyDescriptor>,const Localization&);
    void Assets(EditorSession&, AssetStore&, const Localization&);
    void ImportDialogs(EditorSession&,AssetStore&,const Localization&);
    void RecoveryDialogs(EditorSession&,AssetStore&,UiTree*,const Localization&);
    void PruneRecoveryDocuments(const std::vector<RecoveryDocument>&);
    bool ValidateBuildSources();
    void BuildControlToolbar(EditorSession&,AssetStore&,UiTree*,const Localization&);
    void PreferencesPanel(const Localization&);
    // G15：`.selut` 资产选择、热重载状态与低饱和灰阶/彩色预览。
    void ColorLutSection(const Localization&,const std::filesystem::path& root);
    void BuildDiagnosticsPanel(const Localization&);
    bool CaptureEditingDocuments(const EditorSession&,AssetStore&,UiTree*,std::vector<EditorAssetDocument>&);
    void AssetActionsToolbar(EditorSession&,AssetStore&,const Localization&);
    void AssetActionDialogs(EditorSession&,AssetStore&,const Localization&);
    void ResolvePendingReference(EditorSession&);
    void OpenCodeSource(const std::filesystem::path&,std::uint32_t line=1,std::uint32_t column=1);
    void ProjectFiles(EditorSession&,AssetStore&,const Localization&);
    void SyncAssetDeclarations(AssetStore&);
    void PreviewAsset(AssetStore&,const AssetId&);
    void PollAssetPreview(AssetStore&);
    EditorRect Viewport(EditorSession&, AssetStore&, const Localization&, const EditorViewport&);
    EditorRect UiDesigner(UiTree&,AssetStore&,const Localization&,const EditorViewport&);
    void MaterialEditor(EditorSession&,AssetStore&,const Localization&);
    void TODInspector(EditorSession&,AssetStore&,const Localization&);
    void ProjectSettingsPanel(const Localization&);
    void BuildExportPanel(EditorSession&,AssetStore&,const Localization&,UiTree*);
    void ConsolePanel(const Localization&);
    void AboutPanel(const Localization&);
    void DiagnosticsPanel(const Localization&);
    void LoadProjectSettings(bool discardUnsaved=false);
    void LoadBuildSettings(bool discardUnsaved=false);
    void SyncSettingsDrafts();
    bool SaveSettings(bool project);
    void SettingsReloadDialog(const Localization&);
    void PollExport(const Localization&);
    void LogConsole(int level,const std::string& key,const std::string& detail={});
    void RunEnvironmentDiagnostics(const Localization&);
    bool SaveAllDocuments(EditorSession&,AssetStore&,UiTree*);
    bool DocumentsDirty(UiTree*) const;
    bool UiDirty(UiTree*) const;
    void SyncDocumentRegistry(EditorSession&,UiTree*);
    bool OtherDocumentsDirty() const;
    bool CheckExternalDocument(ExternalDocument,const std::filesystem::path&);
    void RecordStamp(FileStamp&,const std::filesystem::path&) const;
    bool StampChanged(const FileStamp&,const std::filesystem::path&) const;
    std::filesystem::path EditorRoot() const;
    std::filesystem::path ProjectManifestPath() const;
    void OpenInExplorer(const std::filesystem::path& path,bool selectFile=false) const;
    bool OpenMaterial(AssetStore&, const std::filesystem::path&, bool discard = false);
    bool MaterialDirty() const;
    bool SaveMaterial(AssetStore&,bool overwrite=false);
    void Queue(std::function<void()> action) { pending_.push_back(std::move(action)); }
    void BeginUiEdit(UiTree& ui);
    void EndUiEdit(UiTree& ui, bool commit);
    bool UiChanged(UiTree& ui, std::string before);
    void BindUi(UiTree& ui);

    ImGuiContext* context_ = nullptr;
    std::vector<std::uint8_t> fontBytes_;
    std::vector<std::function<void()>> pending_;
    std::array<char,4096> scenePath_{},sceneSavePath_{},importPath_{},uiPath_{},materialPath_{};
    std::array<char, 1024> entityName_{}, uiText_{};
    std::array<char,4096> uiImage_{};
    std::array<char,129> uiBindingScript_{},uiBindingAction_{};
    UiPoint uiReferenceDraft_{},uiReferenceObserved_{};
    UiId bindingNode_=0;int bindingEvent_=0;std::uint64_t bindingEntity_=0;
    StableEntityId inspected_ = 0;
    std::string inspectedName_, errorKey_;
    AssetId selectedAsset_;
    std::shared_ptr<const StaticModel> previewModel_;
    std::shared_ptr<const ImageData> previewImage_;
    struct PreviewResult{AssetId id;std::shared_ptr<const StaticModel> model;std::shared_ptr<const ImageData> image;std::vector<AssetDiagnostic> diagnostics;};
    std::future<PreviewResult> previewTask_;
    AssetId previewPending_;
    std::vector<AssetDiagnostic> assetDiagnostics_;
    MaterialEditSession material_;
    TODPreview todPreview_;
    AssetId activeMaterialId_;
    std::filesystem::path activeMaterialPath_,materialOverwritePath_;
    FileStamp materialStamp_,uiStamp_;
    std::filesystem::path uiSavedPath_;
    ExternalDocument externalDocument_=ExternalDocument::None;
    std::filesystem::path externalDocumentPath_;
    EditorDocumentRegistry documents_;
    SceneFileWorkflow files_;
    bool requestClose_=false,saveAsRequested_=false,saveAsOpen_=false,materialOverwrite_=false,continueCloseAfterMaterial_=false;
    std::string materialField_,inspectorField_;
    enum class DocumentContext { Scene,Material,Ui,ProjectSettings,BuildSettings };
    DocumentContext documentContext_=DocumentContext::Scene;
    void ApplySessionDocumentFocus(const EditorSession&);
    std::optional<EditorPanel> pendingSessionFocus_;
    std::filesystem::path pendingSessionDocument_;
    bool keepWorld_=false;
    bool materialReady_ = false;
    int materialSlot_ = 0;
    EditorGizmo gizmo_;
    GizmoMode mode_ = GizmoMode::Translate;
    GizmoSpace space_=GizmoSpace::World;
    bool inspectorEditing_ = false;
    bool showMaterial_=false,showUi_=false,showHierarchy_=true,showInspector_=true,showAssets_=true,showViewport_=true;
    bool showProjectSettings_=false,showBuild_=false,showConsole_=true,showAbout_=false,showDiagnostics_=false;
    std::filesystem::path dockLayoutPath_;
    bool dockNeedsBuild_=true,dockDirty_=false;
    double dockDirtyAt_=0;
    void SaveLayout();
    void PreparePanel(EditorPanel panel,float x,float y,float width,float height);
    void RecordLayout();
    void DrawDockSpace(const Localization&);
    void BuildDefaultDockLayout(const Localization&);
    void SaveDockLayout();
    void ResetDockLayout();
    EditorLayout layout_;
    EditorCommandRouter commands_;
    EditorPlaybackController playback_;
    std::filesystem::path layoutPath_;
    bool layoutApply_=true,layoutDirty_=false;
    ImVec2 layoutViewport_{};
    double layoutChangedAt_=0;
    float uiScale_=1,dpiScale_=1;
    // 项目设置/构建导出面板状态；字段缓冲只在显式读取或保存后同步。
    std::filesystem::path projectRoot_;
    bool projectSettingsLoaded_=false,buildSettingsLoaded_=false;
    ProjectSettings projectSettings_;
    BuildSettings buildSettings_;
    EditorSettingsDocument projectSettingsDocument_{EditorSettingsKind::Project},buildSettingsDocument_{EditorSettingsKind::Build};
    int settingsReload_=0;
    unsigned projectFieldChanges_=0,buildFieldChanges_=0;
    std::array<char,129> projectNameField_{},buildNameField_{};
    std::array<char,512> contentRootField_{},entrySceneField_{},scriptsProjectField_{},scriptTypeField_{},bindingsField_{},entryUiField_{};
    std::string projectSettingsStatus_,buildSettingsStatus_,projectSettingsError_,buildSettingsError_,diagnosticsReport_;
    std::vector<ConsoleEntry> consoleEntries_;
    int consoleLevelFilter_=0;
    std::array<char,128> consoleSearch_{};
    bool consoleAutoScroll_=true;
    // 导出作业只由 UI 线程启动/查询；工作线程不接触 ImGui 或世界。
    std::unique_ptr<ExportJob> exportJob_;
    ExportJobSnapshot exportSnapshot_;
    bool exportReportLoaded_=false,exportJoined_=true;
    std::uint64_t exportSequence_=0,lastLoggedOperation_=0;
    std::uint64_t workspaceGeneration_=1;
    std::filesystem::path userOperationDirectory_;
    bool uiCanvasMode_=false;
    UICanvasEditor uiCanvas_;
    UiTree* uiOwner_ = nullptr;
    UiId selectedUi_ = 0, inspectedUi_ = 0;
    std::string inspectedUiText_,inspectedUiImage_, uiTransaction_, uiSaved_, importReport_;
    std::vector<std::filesystem::path> importSources_;
    std::future<AssetImportPlan> importPlanning_;
    struct ImportResult{bool success=false;std::vector<AssetId> imported;std::string key,detail;};
    std::future<ImportResult> importExecution_;
    std::optional<AssetImportPlan> importCandidate_;
    std::filesystem::path assetFolder_;
    std::array<char,256> assetSearch_{};
    int assetType_=0;
    bool browseProjectFiles_=false;
    std::array<char,4096> assetTarget_{};
    std::array<char,128> scriptClassName_{'N','e','w','B','e','h','a','v','i','o','u','r','\0'};
    std::future<EditorAssetActionPlan> assetActionPlanning_;
    std::optional<EditorAssetActionPlan> assetActionCandidate_;
    struct AssetActionResult{bool success=false;std::vector<EditorAssetIssue> diagnostics;EditorAssetAction action=EditorAssetAction::Move;std::filesystem::path source,target;};
    std::future<AssetActionResult> assetActionExecution_;
    std::filesystem::path pendingUiOpen_;
    std::optional<EditorAssetReference> pendingAssetReference_;
    std::filesystem::path recoveryDirectory_,recoveryScenePath_;
    std::string recoverySceneDigest_,recoverySignature_;
    std::uint64_t recoveryRevision_=0;
    std::uint64_t recoveryRetryAfter_=0;
    EditorSessionStore::RecoveryDebounce recoveryDebounce_;
    std::vector<RecoveryGroup> recoveryGroups_;
    bool recoveryOffered_=false,recoveryCaptureValid_=true;
    std::vector<std::string> recoveryProtectedGroups_;
    std::string recoveryPrunedSignature_;
    std::filesystem::path restartRequestPath_,buildDiagnosticsPath_;
    std::string buildStatusJson_;
    bool buildStale_=false,allowStaleBuild_=false,recompileRequested_=false,buildHasCandidate_=false;
    EditorExternalDocuments externalDocuments_;
    std::vector<EditorExternalDocument> externalDocumentSnapshots_;
    bool externalDocumentsValid_=true;
    EditorPreferences preferences_;
    std::filesystem::path preferencesDirectory_;
    std::function<bool(const std::string&)> changeLanguage_;
    std::array<char,4096> externalEditorField_{};
    // G15：LUT 面板状态。文件扫描只在显式刷新时发生；预览每帧轮询一次修改时间。
    std::array<char,1024> colorLutField_{};
    std::vector<std::filesystem::path> colorLutAssets_;
    std::string colorLutStatus_;
    se::ColorLutFile colorLutPreview_;
    std::uint64_t colorLutSeenFailure_=0;
    bool colorLutScanned_=false;
    bool showPreferences_=false;
    struct BuildDiagnostic{std::filesystem::path file;std::uint32_t line=1,column=1;std::string message;};
    std::vector<BuildDiagnostic> buildDiagnostics_;
    bool showBuildDiagnostics_=false;
    struct UiCommand { std::string before, after; };
    std::vector<UiCommand> uiUndo_, uiRedo_;
    bool uiDragging_ = false;
};
}
