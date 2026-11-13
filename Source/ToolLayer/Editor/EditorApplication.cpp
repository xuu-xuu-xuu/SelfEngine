#include "EditorApplication.h"
#include "EditorFloatingWindows.h"
#include "EditorWindowStyle.h"
#include "ImGuiEditor.h"
#include "AssetImportService.h"
#include "EditorAssetActions.h"
#include <future>
#include "FunctionLayer/Rendering/UiPaint.h"
#include "ToolLayer/Editor/EditorUi/EditorInputAdapter.h"
#include "ToolLayer/Editor/EditorUi/EditorViewportMath.h"
#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/ProjectSettingsService.h"
#include "ToolLayer/Editor/MaterialThumbnail.h"
#include "ToolLayer/Editor/ExternalChangeMonitor.h"
#include "ToolLayer/Editor/BuildRestartRequest.h"
#include "ToolLayer/Editor/RecoveryApply.h"
#include "ResourceLayer/Assets/MaterialMetadata.h"
#include "PlatformLayer/Windows/UiDirectWriteTextService.h"
#include "EditorSessionStore.h"
#include "EditorSession.h"
#if defined(FLARE_LOCAL_VALIDATION)
#include "EditorSmokeDriver.h"
#endif
#include "ToolLayer/Tools/Project.h"
#include <Windows.h>
#include <commdlg.h>
#include "WindowsEditorInput.h"
#include "EditorPlayInput.h"
#include "ImGuiGeometry.h"
#include "EditorViewportCamera.h"
#include "EditorViewportController.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Log.h"
#include "PlatformLayer/Windows/StartupProgress.h"
#include "FunctionLayer/Rendering/Renderer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <cmath>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace se
{
struct ToolkitSceneImageProvider final : ui::UiImageProvider
{
    std::unordered_map<std::string,ui::UiImageInfo> images;
    bool Resolve(std::string_view image, ui::UiImageInfo& info) const override
    {
        if (image == "scene")
        {
            info.textureToken = 2;
            return true;
        }
        const auto found = images.find(std::string(image));
        if (found == images.end()) return false;
        info = found->second;
        return true;
    }
};
using ui::editor::EditorCommand;
using ui::editor::EditorCommandId;
using ui::editor::EditorInputAdapter;
using ui::editor::EditorPanelFromName;
using ui::editor::EditorPanelId;
using ui::editor::EditorShell;
namespace
{
std::string PropertyText(const PropertyValue& value)
{
    if(const auto* v=std::get_if<bool>(&value))return *v?"true":"false";
    if(const auto* v=std::get_if<float>(&value))return std::to_string(*v);
    if(const auto* v=std::get_if<Vec3>(&value))return std::to_string(v->x)+", "+std::to_string(v->y)+", "+std::to_string(v->z);
    if(const auto* v=std::get_if<Vec4>(&value))return std::to_string(v->x)+", "+std::to_string(v->y)+", "+std::to_string(v->z)+", "+std::to_string(v->w);
    if(const auto* v=std::get_if<Quat>(&value))return std::to_string(v->x)+", "+std::to_string(v->y)+", "+std::to_string(v->z)+", "+std::to_string(v->w);
    if(const auto* v=std::get_if<std::string>(&value))return *v;
    if(const auto* v=std::get_if<PropertyEntityReference>(&value))return std::to_string(v->stableId);
    if(const auto* v=std::get_if<std::vector<std::string>>(&value))
    {
        std::string text;
        for(std::size_t index=0;index<v->size();++index)
        {
            if(index)text+=", ";
            text+=(*v)[index];
        }
        return text;
    }
    return {};
}

bool ParseFloatList(std::string_view text,float* values,std::size_t count)
{
    std::string normalized(text);
    for(char& ch:normalized)if(ch==',')ch=' ';
    std::istringstream stream(normalized);
    for(std::size_t index=0;index<count;++index)if(!(stream>>values[index]))return false;
    return true;
}

bool ParsePropertyValue(const PropertyDescriptor& descriptor,std::string_view text,PropertyValue& output)
{
    try
    {
        switch(descriptor.type)
        {
        case PropertyType::Boolean:
        {
            const bool value=text=="true"||text=="1";
            output=value;
            break;
        }
        case PropertyType::Number:
            output=std::stof(std::string(text));
            break;
        case PropertyType::Vector3:
        {
            float values[3]{};
            if(!ParseFloatList(text,values,3))return false;
            output=Vec3{values[0],values[1],values[2]};
            break;
        }
        case PropertyType::Vector4:
        {
            float values[4]{};
            if(!ParseFloatList(text,values,4))return false;
            output=Vec4{values[0],values[1],values[2],values[3]};
            break;
        }
        case PropertyType::Quaternion:
        {
            float values[4]{};
            if(!ParseFloatList(text,values,4))return false;
            output=Quat{values[0],values[1],values[2],values[3]};
            break;
        }
        case PropertyType::AssetList:
        {
            std::vector<std::string> values;
            std::string item;
            std::istringstream stream{std::string(text)};
            while(std::getline(stream,item,','))
            {
                while(!item.empty()&&(item.front()==' '||item.front()=='\t'))item.erase(item.begin());
                while(!item.empty()&&(item.back()==' '||item.back()=='\t'))item.pop_back();
                if(!item.empty())values.push_back(item);
            }
            output=std::move(values);
            break;
        }
        case PropertyType::EntityReference:
            output=PropertyEntityReference{std::stoull(std::string(text))};
            break;
        case PropertyType::AssetId:
        case PropertyType::Text:
            output=std::string(text);
            break;
        }
    }
    catch(...)
    {
        return false;
    }
    return ValidatePropertyValue(descriptor,output);
}

std::vector<ui::editor::EditorProperty> MaterialRows(const ImGuiEditor& frontend,const Localization& text)
{
    std::vector<ui::editor::EditorProperty> rows;
    if(!frontend.MaterialReadyForToolkit())return rows;
    for(const PropertyDescriptor& descriptor:MaterialProperties())
    {
        PropertyValue value;
        std::string error;
        if(!ReadMaterialProperty(frontend.MaterialForToolkit(),descriptor.key,value,error))continue;
        ui::editor::EditorPropertyType type=ui::editor::EditorPropertyType::String;
        if(descriptor.type==PropertyType::Number)type=ui::editor::EditorPropertyType::Number;
        else if(descriptor.type==PropertyType::Boolean)type=ui::editor::EditorPropertyType::Boolean;
        else if(descriptor.type==PropertyType::AssetId)type=ui::editor::EditorPropertyType::Asset;
        else if(descriptor.type==PropertyType::Vector3||descriptor.type==PropertyType::Vector4)type=ui::editor::EditorPropertyType::Color;
        const std::string label=descriptor.labelKey.empty()?std::string(descriptor.key):text.GetText(descriptor.labelKey);
        rows.push_back({std::string(descriptor.key),label,type,PropertyText(value),!descriptor.editable});
    }
    return rows;
}

bool SameProject(const std::string& stored,const std::filesystem::path& root)
{
    std::error_code error;
    const auto canonical=std::filesystem::weakly_canonical(root,error);
    if(error)return false;
    return stored==asset_io::Utf8(canonical);
}
EditorSessionSnapshot CaptureEditorSession(const std::filesystem::path& root,
    const EditorSession& session,const EditorViewportCamera& camera,const ImGuiEditor& frontend)
{
    EditorSessionSnapshot snapshot;
    std::error_code error;
    const auto canonical=std::filesystem::weakly_canonical(root,error);
    if(error)return snapshot;
    snapshot.project=asset_io::Utf8(canonical);
    snapshot.documents=frontend.SessionDocuments(session);
    snapshot.activeDocument=frontend.SessionActiveDocument(session);
    if(session.Selected())snapshot.selection.push_back(session.Selected());
    snapshot.cameraX=camera.state.position.x;snapshot.cameraY=camera.state.position.y;snapshot.cameraZ=camera.state.position.z;
    snapshot.cameraYaw=camera.state.yaw;snapshot.cameraPitch=camera.state.pitch;
    snapshot.layoutVersion=1;
    return snapshot;
}
}
int RunEditorApp(Renderer& renderer,const std::string& locale,const std::filesystem::path& requestedRoot,
    bool hidden,std::uint64_t maximumFrames,EditorApplicationHooks* hooks)
{
    struct CrashMarkerGuard
    {
        std::filesystem::path directory;bool active=false;
        ~CrashMarkerGuard(){if(active){std::string error;EditorSessionStore::ClearCrashMarker(directory,error);}}
    } crashGuard;
    try
    {
        const auto root=std::filesystem::weakly_canonical(requestedRoot);
#if defined(FLARE_LOCAL_VALIDATION)
        auto smoke=EditorSmokeDriver::FromEnvironment(root);
#endif
        const bool toolkit=hooks&&hooks->editorUi=="toolkit";
        std::unique_ptr<EditorShell> toolkitShell;
        std::unique_ptr<EditorInputAdapter> toolkitInput;
        std::unique_ptr<EditorFloatingWindows> floatingWindows;
        bool toolkitGameplayLogged=false;
        std::optional<CameraSnapshot> toolkitCamera;
        ui::UiRect toolkitViewportRect;
        std::unique_ptr<UiDirectWriteTextService> toolkitText;
        std::vector<unsigned char> toolkitFontPixels;
        int toolkitFontWidth=0,toolkitFontHeight=0;
        std::uint64_t toolkitFontRevision=0;
        ProjectSettings toolkitProjectSettings;
        BuildSettings toolkitBuildSettings;
        bool toolkitExportBusy=false;
        bool toolkitExportStarted=false;
        bool toolkitExportFinished=false;
        std::string toolkitExportName="GameExport";
        bool toolkitDpiOverride=false;
        bool toolkitAutoExport=false;
        bool toolkitAutoExportStarted=false;
        bool toolkitAutoExportFlow=false;
        int toolkitAutoExportFlowPhase=0;
        bool toolkitAutoCancelRequested=false;
        std::uint64_t toolkitAutoExportStart=0;
        int toolkitAutoCancelMs=0;
        std::function<bool()> toolkitAutoExportFn;
        std::shared_ptr<const ImageData> toolkitPreviewImage;
        std::string toolkitPreviewImageKey;
        std::uint64_t toolkitPreviewToken=0;
        std::unordered_map<std::string,ui::UiImageInfo> toolkitImageInfos;
        ExternalChangeMonitor toolkitExternalChange;
        std::string toolkitPendingAsset;
        struct ToolkitImportResult
        {
            bool success=false;
            std::string key,detail;
            std::filesystem::path primary;
        };
        std::future<ToolkitImportResult> toolkitImportTask;
        std::string toolkitImportStatus;
        std::filesystem::path toolkitUiPath;
        std::function<bool()> toolkitExportAction;
        bool toolkitProjectDirty=false, toolkitBuildDirty=false, toolkitUiDirty=false;
        if(!std::filesystem::is_directory(root)){Log(LogLevel::Error,"EditorUi.InvalidProject");return 1;}
        crashGuard.directory=EditorSessionStore::ProjectDirectory(root);
        EditorSessionStore::CrashMarker previous;std::string startupError;
        const auto startup=EditorSessionStore::InspectStartup(crashGuard.directory,previous,startupError);
        if(startup==EditorSessionStore::StartupState::ActiveInstance){Log(LogLevel::Warning,"Editor.AnotherInstance");return 1;}
        if(startup==EditorSessionStore::StartupState::UncleanExit)Log(LogLevel::Warning,"Editor.UncleanExit");
        std::string cleanupError;
        if(!EditorSessionStore::CleanupAbandonedCandidates(crashGuard.directory,cleanupError))Log(LogLevel::Warning,cleanupError);
        EditorSessionStore::CrashMarker marker;marker.processId=GetCurrentProcessId();marker.processCreation=EditorSessionStore::CurrentProcessCreation();
        std::string markerError;crashGuard.active=EditorSessionStore::WriteCrashMarker(crashGuard.directory,marker,markerError);
        if(!crashGuard.active)Log(LogLevel::Warning,markerError);
        const bool safeMode=GetEnvironmentVariableW(L"SELFENGINE_EDITOR_SAFE_MODE",nullptr,0)>0;
        EditorPreferences preferences;std::string preferencesError;
        EditorSessionStore::LoadPreferences(EditorSessionStore::UserDataDirectory(),preferences,preferencesError);
        auto resourceDirectory=root/"Resources";
        if(hooks&&!hooks->resourceDirectory.empty())resourceDirectory=hooks->resourceDirectory;
        if(!std::filesystem::is_directory(resourceDirectory/"Localization"))resourceDirectory=GetExecutableDirectory()/"Resources";
        Localization text;std::string error;
        if(!text.Load(resourceDirectory/"Localization"/(locale+".txt"),error))
        {
            Log(LogLevel::Warning,"Language "+locale+": "+error);
            if(locale!="en-US")text.Load(resourceDirectory/"Localization"/"en-US.txt",error);
        }
        auto assetsRoot=std::filesystem::is_directory(root/"Content")?root/"Content"
            :std::filesystem::is_directory(root/"Assets")?root/"Assets"
            :std::filesystem::is_directory(root/"Resources")?root/"Resources":root;
        if(std::filesystem::exists(root/"project.json"))
        {
            ProjectDescription project;std::vector<ProjectDiagnostic> failures;
            if(!ReadProject(root/"project.json",project,failures))
            {
                for(const auto& failure:failures)Log(LogLevel::Error,text.GetText(failure.key)+": "+failure.detail);
                return 1;
            }
            assetsRoot=project.contentRoot;
        }
        AssetStore assets(assetsRoot,root/"out"/"Cache"/"Assets");
        if(!ReportStartup(StartupStage::Assets))return 1;
        std::vector<AssetDiagnostic> diagnostics;
        if(!assets.Scan(diagnostics))
        {
            for(const auto& diagnostic:diagnostics)Log(LogLevel::Error,text.GetText(diagnostic.key)+": "+diagnostic.detail);
            return 1;
        }
        Log(LogLevel::Info,"EditorUi.Assets: "+std::to_string(assets.ListIds().size()));
        EditorSession session;
        session.SetTODResolver([&assets](const std::string& id,std::string& error){
            std::vector<AssetDiagnostic> failures;auto profile=assets.LoadTODProfile(id,failures);
            if(!profile)error=failures.empty()?"TOD.ResolveFailed":failures.back().key;return profile;
        });
        EditorSessionSnapshot restoredSession;std::string sessionError;
        const bool hasRestoredSession=!safeMode&&EditorSessionStore::LoadSession(crashGuard.directory,restoredSession,sessionError)
            &&SameProject(restoredSession.project,root);
        UiTree ui;
        std::unique_ptr<UiTree> playUi;
        std::unique_ptr<EditorPlayInput> playInput;
        // guard 在 ui 之后析构之前执行，确保脚本停止回调可继续访问实际运行 UI。
        struct PlayLifetime { EditorSession& session; ~PlayLifetime(){session.StopPlay();session.SetPlayCallbacks({},{});} } playLifetime{session};
        if(!ReportStartup(StartupStage::Scene))return 1;
        if(hooks&&hooks->initialize&&!hooks->initialize(session,ui))return 1;
        // 产品入口由 EditorWorker 负责缺失/空场景的内存默认；测试夹具可继续使用空会话。
        // 没有显式场景参数时，恢复上次会话的活动场景；缺失文件只警告，不创建空文件。
        const bool managedDefaultScene=GetEnvironmentVariableW(L"SELFENGINE_EDITOR_SCENE_FILE",nullptr,0)>0
            &&GetEnvironmentVariableW(L"SELFENGINE_EDITOR_SCENE_EXPLICIT",nullptr,0)==0;
        if(hasRestoredSession&&(session.Path().empty()||managedDefaultScene)&&!restoredSession.documents.empty())
        {
            for(const auto& document:restoredSession.documents)if(document.ends_with(".scene.json"))
            {
                const auto requested=asset_io::FromUtf8(document);const auto path=requested.is_absolute()?requested:root/requested;
                if(!asset_io::Within(root,path)||!session.Open(path,true))Log(LogLevel::Warning,"Editor.SessionDocumentMissing");
                break;
            }
        }
        session.SetPlayCallbacks([&](World& world){
            playUi=std::make_unique<UiTree>(ui);
            playUi->CancelInput();
            if(playInput)playInput->Attach(playUi.get());
            return !hooks||!hooks->playStarted||hooks->playStarted(world,*playUi);
        },[&](World& world){
            if(playInput)playInput->Attach(nullptr);
            struct ClearUi{std::unique_ptr<UiTree>& value;~ClearUi(){value.reset();}} clearUi{playUi};
            if(hooks&&hooks->playStopping&&playUi)hooks->playStopping(world,*playUi);
        });
        ImGuiEditor frontend;
        frontend.SetAuxiliaryPanels(false);
        frontend.SetLayoutPath(safeMode?std::filesystem::path{}:crashGuard.directory/"layout.json");
        frontend.SetDockLayoutPath(safeMode?std::filesystem::path{}:crashGuard.directory/"editor-dock.ini");
        frontend.SetUiScale(preferences.uiScale);
        EditorViewportCamera editorCamera;
        EditorViewportController viewportController(editorCamera);
        if(hasRestoredSession)
        {
            if(std::isfinite(restoredSession.cameraX)&&std::isfinite(restoredSession.cameraY)&&std::isfinite(restoredSession.cameraZ)
                &&std::isfinite(restoredSession.cameraYaw)&&std::isfinite(restoredSession.cameraPitch))
            {
                editorCamera.state.position={restoredSession.cameraX,restoredSession.cameraY,restoredSession.cameraZ};
                editorCamera.state.yaw=restoredSession.cameraYaw;editorCamera.state.pitch=restoredSession.cameraPitch;
            }
            if(!restoredSession.selection.empty())session.Select(restoredSession.selection.front());
        }
        std::uint64_t editorFrame=0;
        bool navigating=false,navigationCancelled=false;
        playInput=std::make_unique<EditorPlayInput>(frontend.Context());
        if(hooks){frontend.SetScriptInspector(hooks->inspector);frontend.SetExternalDocuments(hooks->externalDocuments);}
        frontend.SetProjectRoot(root);
        preferences.language=locale;
        frontend.ConfigurePreferences(preferences,EditorSessionStore::UserDataDirectory(),[&](const std::string& language){
            std::string failure;return text.Load(resourceDirectory/"Localization"/(language+".txt"),failure);
        });
        frontend.SetDocumentPaths(assetsRoot/"Scenes"/"Untitled.scene.json",assetsRoot/"UI"/"Untitled.ui.json");
        if(hooks&&hooks->configure)hooks->configure(frontend);
        if(hasRestoredSession)frontend.RestoreSessionDocuments(restoredSession.documents,assets,ui,restoredSession.activeDocument);
        frontend.ConfigureRecovery(crashGuard.directory,!safeMode&&!hidden);
        frontend.ConfigureBuildControl();
        auto observedSession=CaptureEditorSession(root,session,editorCamera,frontend);
        auto persistedSession=hasRestoredSession?restoredSession:EditorSessionSnapshot{};
        auto sessionChangedAt=std::chrono::steady_clock::now();
        const auto persistSession=[&](bool force){
            const auto current=CaptureEditorSession(root,session,editorCamera,frontend);const auto now=std::chrono::steady_clock::now();
            if(current!=observedSession){observedSession=current;sessionChangedAt=now;}
            if(current!=persistedSession&&(force||now-sessionChangedAt>=std::chrono::seconds(1)))
            {std::string failure;if(EditorSessionStore::SaveSession(crashGuard.directory,current,failure))persistedSession=current;else{Log(LogLevel::Warning,failure);sessionChangedAt=now;}}
        };
        wchar_t windowsDirectory[MAX_PATH]{};
        bool fontLoaded=false;
        const auto length=GetWindowsDirectoryW(windowsDirectory,MAX_PATH);
        if(length>0&&length<MAX_PATH)
        {
            const auto fonts=std::filesystem::path(windowsDirectory)/"Fonts";
            for(const auto* name:{L"msyh.ttc",L"msyh.ttf",L"simhei.ttf"})
                if(std::filesystem::is_regular_file(fonts/name)&&frontend.LoadFont(fonts/name))
                {fontLoaded=true;break;}
        }
        if(!fontLoaded)Log(LogLevel::Warning,text.GetText("EditorUi.FontLoadFailed"));
        // 固定图集由通用 GUI 后端首次上传；ImGui 不持有 native 描述符。
        const unsigned char* fontPixels=nullptr;int fontWidth=0,fontHeight=0;
        frontend.FontAtlasRgba(fontPixels,fontWidth,fontHeight);
        frontend.SetFontTexture(1); // 仅 CPU token，renderer 映射为自己的真实 GPU 资源。
        std::unique_ptr<WindowsEditorInput> platform;
        HWND hostWindow=nullptr;
        ApplicationServices services;
        services.resourceDirectory=resourceDirectory;
        services.maximumFrames=maximumFrames;
        services.initialWidth=1280;services.initialHeight=760;
        // 不设置 services.ui：设计用 UI 文档由 ImGui 画布编辑，不重复路由到游戏 UI。
        services.windowOpened=[&](HWND window){
            if(!ReportStartup(StartupStage::Interface))return false;
            SetWindowTextW(window,asset_io::FromUtf8(text.GetText("Editor.WindowTitle")).c_str());
            flare::StyleTitleBar(window);
            hostWindow=window;
            if(toolkit)
            {
                toolkitText=std::make_unique<UiDirectWriteTextService>(L"Segoe UI");
                toolkitShell=std::make_unique<EditorShell>(toolkitText.get());
                toolkitShell->SetLocalization(&text);
                if(!toolkitShell->Initialize(1280.0f,760.0f))
                {
                    Log(LogLevel::Error,"EditorUi.ToolkitInitFailed");
                    toolkitShell.reset();
                    return false;
                }
                toolkitShell->SetSelectionHandler([&session](std::uint64_t id){session.Select(id);Log(LogLevel::Info,"EditorUi.ToolkitSelection: "+std::to_string(id));});
                toolkitShell->SetViewportImage("scene");
                toolkitShell->SetLayoutPath(safeMode?std::filesystem::path{}:crashGuard.directory/".editor-ui.json");
                {
                    ProjectDescription description;
                    std::vector<ProjectDiagnostic> projectDiagnostics;
                    if(ReadProject(root/"project.json",description,projectDiagnostics)&&!description.uiDocuments.empty())
                    {
                        const auto& uiPath=description.uiDocuments.front();
                        std::ifstream input(uiPath,std::ios::binary);
                        if(input)
                        {
                            const std::string json{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
                            std::string uiError;
                            if(ui.LoadJson(json,uiError))
                            {
                                toolkitUiPath=uiPath;
                                Log(LogLevel::Info,"EditorUi.ToolkitUi: loaded "+asset_io::Utf8(uiPath));
                            }
                            else
                            {
                                Log(LogLevel::Warning,"EditorUi.ToolkitUi: "+uiError);
                            }
                        }
                    }
                }
                toolkitShell->SetDesignerHandlers(
                    [&frontend,&ui,&toolkitShell,&text](float x,float y){
                        const UiId hit=frontend.UiCanvas().Hit(ui,{x,y});
                        if(!hit)return;
                        frontend.SelectUiForToolkit(hit);
                        if(!frontend.BeginUiCanvasDragForToolkit(ui,hit,{x,y}))
                            toolkitShell->SetStatus(text.GetText("EditorUi.DesignerDragFailed"));
                    },
                    [&frontend,&ui,&toolkitUiDirty](float x,float y){
                        if(frontend.UiCanvas().Active()&&frontend.UpdateUiCanvasDragForToolkit(ui,{x,y}))
                            toolkitUiDirty=true;
                    },
                    [&frontend,&ui,&toolkitUiPath,&toolkitUiDirty,&toolkitShell,&text](float,float){
                        if(!frontend.UiCanvas().Active())return;
                        if(frontend.EndUiCanvasDragForToolkit(ui,true))
                        {
                            toolkitUiDirty=true;
                            if(!toolkitUiPath.empty())
                            {
                                const std::string json=ui.SaveJson();
                                std::ofstream output(toolkitUiPath,std::ios::binary|std::ios::trunc);
                                output<<json;
                                if(output)
                                {
                                    toolkitUiDirty=false;
                                    Log(LogLevel::Info,"EditorUi.ToolkitUi: saved "+asset_io::Utf8(toolkitUiPath));
                                }
                                else
                                {
                                    toolkitShell->SetStatus(text.GetText("EditorUi.SaveFailed"));
                                }
                            }
                            else
                            {
                                toolkitShell->SetStatus(text.GetText("EditorUi.SaveFailed"));
                            }
                        }
                    });
                toolkitShell->SetAssetSelectionHandler([&assets,&frontend,&toolkitShell,&text,
                    &toolkitPreviewImage,&toolkitPreviewImageKey,&toolkitPreviewToken,&toolkitImageInfos](std::string path){
                    Log(LogLevel::Info,"EditorUi.ToolkitAsset: "+path);
                    const std::filesystem::path selected=asset_io::FromUtf8(path);
                    toolkitShell->SetAssetsPreviewText({});
                    toolkitShell->SetAssetPreviewImage({},0.0f,0.0f);
                    toolkitImageInfos.clear();
                    toolkitPreviewImage.reset();
                    toolkitPreviewImageKey.clear();
                    toolkitPreviewToken=0;
                    if(!IsMaterialDocumentPath(selected))
                    {
                        const std::string extension=asset_io::Utf8(selected.extension());
                        if(extension==".png"||extension==".jpg"||extension==".jpeg"||extension==".bmp"||extension==".tga")
                        {
                            for(const AssetId& id:assets.ListIds())
                            {
                                std::error_code error;
                                if(!std::filesystem::equivalent(assets.Resolve(id),selected,error)||error)continue;
                                std::vector<AssetDiagnostic> imageDiagnostics;
                                auto image=assets.LoadTextureImage(id,imageDiagnostics);
                                if(!image||image->width==0||image->height==0)break;
                                const std::size_t hash=std::hash<std::string>{}(path);
                                toolkitPreviewToken=0x100000000ull|(static_cast<std::uint64_t>(hash)&0xffffffffull);
                                toolkitPreviewImage=std::move(image);
                                toolkitPreviewImageKey=path;
                                toolkitImageInfos[path]={toolkitPreviewToken,static_cast<float>(toolkitPreviewImage->width),
                                    static_cast<float>(toolkitPreviewImage->height)};
                                toolkitShell->SetAssetPreviewImage(path,static_cast<float>(toolkitPreviewImage->width),
                                    static_cast<float>(toolkitPreviewImage->height));
                                Log(LogLevel::Info,"EditorUi.ToolkitAssetPreview: "+path);
                                break;
                            }
                        }
                        return;
                    }
                    std::string materialError;
                    if(frontend.OpenMaterialForToolkit(assets,selected,false,materialError))
                    {
                        toolkitShell->SetFormProperties(EditorPanelId::Material,MaterialRows(frontend,text));
                        toolkitShell->SetFormStatus(EditorPanelId::Material,"Opened "+path);
                        toolkitShell->FocusPanel(EditorPanelId::Material);
                        std::string summary=text.GetText("EditorUi.MaterialPreview")+": "+path;
                        for(const char* key:{"baseColor","metallic","roughness"})
                        {
                            PropertyValue value;
                            if(ReadMaterialProperty(frontend.MaterialForToolkit(),key,value,materialError))
                                summary+=std::string(" | ")+key+"="+PropertyText(value);
                        }
                        toolkitShell->SetAssetsPreviewText(std::move(summary));
                        Vec4 baseColor{0.8f, 0.8f, 0.8f, 1.0f};
                        float metallic = 0.0f, roughness = 0.5f;
                        PropertyValue value;
                        if(ReadMaterialProperty(frontend.MaterialForToolkit(),"baseColor",value,materialError))
                            if(const auto* color=std::get_if<Vec4>(&value))baseColor=*color;
                        if(ReadMaterialProperty(frontend.MaterialForToolkit(),"metallic",value,materialError))
                            if(const auto* number=std::get_if<float>(&value))metallic=*number;
                        if(ReadMaterialProperty(frontend.MaterialForToolkit(),"roughness",value,materialError))
                            if(const auto* number=std::get_if<float>(&value))roughness=*number;
                        const MaterialThumbnail thumbnail=RenderMaterialThumbnail(baseColor,metallic,roughness,64);
                        auto image=std::make_shared<ImageData>();
                        image->width=thumbnail.width;image->height=thumbnail.height;image->rgba8=thumbnail.rgba8;
                        const std::string imageKey="material:"+path;
                        const std::size_t hash=std::hash<std::string>{}(imageKey);
                        toolkitPreviewToken=0x100000000ull|(static_cast<std::uint64_t>(hash)&0xffffffffull);
                        toolkitPreviewImage=std::move(image);
                        toolkitPreviewImageKey=imageKey;
                        toolkitImageInfos[imageKey]={toolkitPreviewToken,static_cast<float>(toolkitPreviewImage->width),
                            static_cast<float>(toolkitPreviewImage->height)};
                        toolkitShell->SetAssetPreviewImage(imageKey,static_cast<float>(toolkitPreviewImage->width),
                            static_cast<float>(toolkitPreviewImage->height));
                        Log(LogLevel::Info,"EditorUi.ToolkitMaterial: opened "+path);
                    }
                    else
                    {
                        toolkitShell->SetFormStatus(EditorPanelId::Material,
                            materialError.empty()?"Open material failed":materialError);
                        toolkitShell->FocusPanel(EditorPanelId::Material);
                        Log(LogLevel::Warning,"EditorUi.ToolkitMaterial: failed "+materialError);
                    }
                });
                const auto toolkitImport=[&](){
                    const auto report=[&](const char* key,const std::string& detail={}){
                        toolkitImportStatus=text.GetText(key)+(detail.empty()?std::string{}:": "+detail);
                        toolkitShell->SetStatus(toolkitImportStatus);
                        toolkitShell->SetAssetsPreviewText(toolkitImportStatus);
                        toolkitShell->AppendConsoleLine(toolkitImportStatus);
                    };
                    if(toolkitImportTask.valid()||frontend.AssetOperationBusy()||frontend.ExportBusy())
                    {report("Editor.AssetOperationBusy");return false;}
                    if(session.IsPlaying()){report("Editor.EditWhilePlaying");return false;}
                    std::filesystem::path source;
                    if(hooks&&hooks->chooseAssetImportSource)source=hooks->chooseAssetImportSource();
                    else
                    {
                        wchar_t file[32768]{};
                        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=hostWindow;
                        dialog.lpstrFilter=L"Assets\0*.fbx;*.gltf;*.glb;*.png;*.jpg;*.jpeg;*.material.json;*.sebin\0All files\0*.*\0";
                        dialog.lpstrFile=file;dialog.nMaxFile=static_cast<DWORD>(std::size(file));
                        const std::wstring initial=assets.SourceRoot().wstring();dialog.lpstrInitialDir=initial.c_str();
                        dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
                        if(!GetOpenFileNameW(&dialog))
                        {
                            const DWORD error=CommDlgExtendedError();
                            if(error)report("Editor.AssetImportFailed",std::to_string(error));
                            return false;
                        }
                        source=file;
                    }
                    if(source.empty())return false;
                    report("Editor.AssetImporting",asset_io::Utf8(source.filename()));
                    const AssetImportRequest request{{source},assets.SourceRoot(),{}};
                    // 工作线程只使用自有 Store；实时索引和 UI 在完成后由主线程更新。
                    toolkitImportTask=std::async(std::launch::async,[request,cache=root/"out"/"Cache"/"Assets"]{
                        ToolkitImportResult result;
                        const auto plan=AssetImportService::Plan(request);
                        if(!plan.valid)
                        {
                            result.key=plan.errorKey;result.detail=plan.detail;
                            if(!plan.diagnostics.empty())
                            {result.key=plan.diagnostics.front().key;result.detail=plan.diagnostics.front().detail;}
                            return result;
                        }
                        for(const auto& file:plan.files)if(file.primary){result.primary=file.target;break;}
                        AssetStore worker(plan.contentRoot,cache);
                        std::vector<AssetId> imported;
                        result.success=AssetImportService::ExecuteRegistered(plan,worker,imported,result.key,result.detail);
                        if(result.success)result.detail=std::to_string(imported.size())+" | "+asset_io::Utf8(result.primary);
                        return result;
                    });
                    return true;
                };
                toolkitShell->SetAssetImportHandler([toolkitImport](){toolkitImport();});
                toolkitShell->SetViewportActionHandler([&](std::string_view action){viewportController.Action(action,session,assets);});
                toolkitShell->SetReparentHandler([&session](std::uint64_t id,std::uint64_t parent){
                    if(!session.BeginEdit("Reparent"))return false;
                    if(!session.SetParent(id,parent)){session.CancelEdit();return false;}
                    const bool committed=session.CommitEdit();
                    if(committed)Log(LogLevel::Info,"EditorUi.ToolkitReparent: "+std::to_string(id)+" -> "+std::to_string(parent));
                    return committed;
                });
                toolkitShell->SetPropertyHandler([&session](std::uint64_t id,std::string key,std::string value){
                    if(key=="name")
                    {
                        if(!session.BeginEdit("Rename"))return false;
                        if(!session.Rename(id,value)){session.CancelEdit();return false;}
                        const bool committed=session.CommitEdit();
                        if(committed)Log(LogLevel::Info,"EditorUi.ToolkitProperty: name");
                        return committed;
                    }
                    if(key=="mesh.asset")
                    {
                        const EntityHandle handle=session.EditWorld().FindByStableId(id);
                        const MeshComponent* current=session.EditWorld().GetMesh(handle);
                        if(!current)return false;
                        MeshComponent next=*current;next.meshAsset=value;
                        if(!session.BeginEdit("Mesh"))return false;
                        if(!session.SetMesh(id,next)){session.CancelEdit();return false;}
                        return session.CommitEdit();
                    }
                    if(key=="mesh.material")
                    {
                        const EntityHandle handle=session.EditWorld().FindByStableId(id);
                        const MeshComponent* current=session.EditWorld().GetMesh(handle);
                        if(!current)return false;
                        MeshComponent next=*current;
                        if(next.materials.empty())next.materials.push_back(value);else next.materials.front()=value;
                        if(!session.BeginEdit("Material"))return false;
                        if(!session.SetMesh(id,next)){session.CancelEdit();return false;}
                        return session.CommitEdit();
                    }
                    if(key=="environment.todProfile"||key=="environment.preserveTime")
                    {
                        SceneEnvironmentSettings settings=session.EditWorld().EnvironmentSettings();
                        if(key=="environment.todProfile")settings.todProfile=value;
                        else settings.preserveTime=(value=="true");
                        return session.Apply("TOD.Editor",[settings](World& world){return world.SetEnvironmentSettings(settings);});
                    }
                    if(key=="camera.fov"||key=="light.intensity")
                    {
                        float parsed=0.0f;
                        try{std::size_t used=0;parsed=std::stof(value,&used);if(used!=value.size())return false;}catch(...){return false;}
                        const EntityHandle handle=session.EditWorld().FindByStableId(id);
                        if(key=="camera.fov")
                        {
                            const CameraComponent* current=session.EditWorld().GetCamera(handle);
                            if(!current)return false;
                            CameraComponent next=*current;next.verticalFovRadians=parsed;
                            return session.Apply("Camera",[id,next](World& world){return world.SetCamera(world.FindByStableId(id),next);});
                        }
                        const LightComponent* current=session.EditWorld().GetLight(handle);
                        if(!current)return false;
                        LightComponent next=*current;next.intensity=parsed;
                        return session.Apply("Light",[id,next](World& world){return world.SetLight(world.FindByStableId(id),next);});
                    }
                    const bool isPosition=key.rfind("position.",0)==0;
                    const bool isScale=key.rfind("scale.",0)==0;
                    if(!isPosition&&!isScale)return false;
                    float parsed=0.0f;
                    try{std::size_t used=0;parsed=std::stof(value,&used);if(used!=value.size())return false;}catch(...){return false;}
                    const EntityHandle handle=session.EditWorld().FindByStableId(id);
                    const Transform* current=session.EditWorld().GetTransform(handle);
                    if(!current)return false;
                    Transform next=*current;
                    const int axis=key.back()=='x'?0:(key.back()=='y'?1:2);
                    if(isPosition)(axis==0?next.position.x:axis==1?next.position.y:next.position.z)=parsed;
                    else (axis==0?next.scale.x:axis==1?next.scale.y:next.scale.z)=parsed;
                    if(!session.BeginEdit("Transform"))return false;
                    if(!session.SetTransform(id,next)){session.CancelEdit();return false;}
                    return session.CommitEdit();
                });
                {
                    auto& commands=toolkitShell->Commands();
                    const auto saveScene=[&session,&assetsRoot,&toolkitExternalChange](){
                        const std::filesystem::path path=session.Path().empty()
                            ?assetsRoot/"Scenes"/"Untitled.scene.json":session.Path();
                        const bool saved=session.Save(path);
                        if(saved)toolkitExternalChange.Reset(session.Path());
                        return saved;
                    };
                    const auto saveAll=[&frontend,&assets,&session,saveScene,&root,&toolkitShell,
                        &toolkitProjectSettings,&toolkitBuildSettings,&toolkitProjectDirty,&toolkitBuildDirty,
                        &toolkitUiDirty,&toolkitUiPath,&ui](){
                        if(frontend.MaterialDirtyForToolkit())
                        {
                            std::string error;
                            if(!frontend.SaveMaterialForToolkit(assets,error)){Log(LogLevel::Warning,"EditorUi.ToolkitSaveMaterial: "+error);return false;}
                        }
                        if(toolkitProjectDirty)
                        {
                            std::string error;
                            if(!ProjectSettingsService::WriteProject(root/"project.json",toolkitProjectSettings,error))
                            {
                                toolkitShell->SetFormStatus(EditorPanelId::ProjectSettings,error);return false;
                            }
                            toolkitProjectDirty=false;
                        }
                        if(toolkitBuildDirty)
                        {
                            std::string error;
                            if(!ProjectSettingsService::WriteBuild(root/"build.json",toolkitBuildSettings,error))
                            {
                                toolkitShell->SetFormStatus(EditorPanelId::BuildExport,error);return false;
                            }
                            toolkitBuildDirty=false;
                        }
                        if(toolkitUiDirty)
                        {
                            if(toolkitUiPath.empty()||!std::filesystem::is_regular_file(toolkitUiPath))
                            {
                                toolkitShell->SetStatus("UI document path is unavailable");
                                return false;
                            }
                            const std::string json=ui.SaveJson();
                            std::ofstream output(toolkitUiPath,std::ios::binary|std::ios::trunc);
                            output<<json;
                            if(!output){toolkitShell->SetStatus("UI document save failed");return false;}
                            toolkitUiDirty=false;
                        }
                        return !session.Dirty()||saveScene();
                    };
                    commands.Register(EditorCommandId::Save,[saveAll](const EditorCommand&){return saveAll();});
                    commands.Register(EditorCommandId::SaveAll,[saveAll](const EditorCommand&){return saveAll();});
                    commands.Register(EditorCommandId::Open,[&session,&toolkitShell,&text,&toolkitExternalChange](const EditorCommand&){
                        if(session.Dirty())
                        {
                            toolkitShell->SetStatus(text.GetText("EditorUi.SaveFirst"));
                            return false;
                        }
                        wchar_t file[MAX_PATH]{};
                        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=GetActiveWindow();
                        dialog.lpstrFilter=L"Flare Scene\0*.scene.json\0All Files\0*.*\0";
                        dialog.lpstrFile=file;dialog.nMaxFile=MAX_PATH;dialog.lpstrDefExt=L"scene.json";
                        dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
                        if(!GetOpenFileNameW(&dialog))return true;
                        if(!session.Open(file,true)){toolkitShell->SetStatus(session.LastError());return false;}
                        toolkitExternalChange.Reset(session.Path());
                        Log(LogLevel::Info,"EditorUi.ToolkitScene: opened");
                        return true;
                    });
                    commands.Register(EditorCommandId::NewScene,[&session,&toolkitShell,&text,&toolkitExternalChange](const EditorCommand&){
                        if(session.Dirty())
                        {
                            toolkitShell->SetStatus(text.GetText("EditorUi.SaveFirst"));
                            return false;
                        }
                        std::string error;
                        if(!session.NewDefaultScene(error)){toolkitShell->SetStatus(error);return false;}
                        toolkitExternalChange.Reset(session.Path());
                        Log(LogLevel::Info,"EditorUi.ToolkitScene: new");
                        return true;
                    });
                    commands.Register(EditorCommandId::SaveAs,[&session,&toolkitShell,&toolkitExternalChange](const EditorCommand&){
                        wchar_t file[MAX_PATH]{};
                        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=GetActiveWindow();
                        dialog.lpstrFilter=L"Flare Scene\0*.scene.json\0All Files\0*.*\0";
                        dialog.lpstrFile=file;dialog.nMaxFile=MAX_PATH;dialog.lpstrDefExt=L"scene.json";
                        if(!session.Path().empty())wcsncpy_s(file,session.Path().c_str(),_TRUNCATE);
                        dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
                        if(!GetSaveFileNameW(&dialog))return true;
                        if(!session.Save(file)){toolkitShell->SetStatus(session.LastError());return false;}
                        toolkitExternalChange.Reset(session.Path());
                        Log(LogLevel::Info,"EditorUi.ToolkitScene: saved-as");
                        return true;
                    });
                    toolkitExportAction=[&frontend,&toolkitShell,&text,&toolkitExportStarted,&toolkitExportFinished,&toolkitExportName,saveAll](){
                        if(frontend.ExportBusy())
                        {
                            frontend.RequestCancelExport();
                            Log(LogLevel::Info,"EditorUi.ToolkitExport: cancel");
                            toolkitShell->SetFormStatus(EditorPanelId::BuildExport,"Cancelling export");
                            return true;
                        }
                        if(!saveAll())
                        {
                            toolkitShell->SetFormStatus(EditorPanelId::BuildExport,"Save failed; export cancelled");
                            return false;
                        }
                        std::string error;
                        toolkitExportStarted=false;
                        toolkitExportFinished=false;
                        const std::string base=toolkitExportName.empty()?"GameExport":toolkitExportName;
                        const auto packageExists=[&base](const std::string& candidate){
                            const auto packages=std::filesystem::current_path()/"packages";
                            return std::filesystem::exists(packages/candidate)||
                                std::filesystem::exists(std::filesystem::current_path()/".cache"/"packages"/candidate)||
                                std::filesystem::exists(std::filesystem::current_path()/"out"/"packages"/candidate);
                        };
                        std::string exportName=base;
                        for(int suffix=2;packageExists(exportName)&&suffix<1000;++suffix)
                            exportName=base+"-"+std::to_string(suffix);
                        if(!frontend.StartExport(exportName,error))
                        {
                            toolkitShell->SetFormStatus(EditorPanelId::BuildExport,error);
                            return false;
                        }
                        toolkitShell->SetFormStatus(EditorPanelId::BuildExport,text.GetText("EditorUi.ExportStarted"));
                        toolkitExportStarted=true;
                        Log(LogLevel::Info,"EditorUi.ToolkitExport: start");
                        return true;
                    };
                    commands.Register(EditorCommandId::Export,[&toolkitExportAction](const EditorCommand&){return toolkitExportAction();});
                    commands.Register(EditorCommandId::Build,[&saveAll,&toolkitShell,&text,&hostWindow](const EditorCommand&){
                        wchar_t requestPath[MAX_PATH]{};
                        const DWORD length=GetEnvironmentVariableW(L"SELFENGINE_EDITOR_RESTART_REQUEST",requestPath,MAX_PATH);
                        if(length==0||length>=MAX_PATH)
                        {
                            toolkitShell->SetStatus(text.GetText("EditorUi.BuildControlUnavailable"));
                            return false;
                        }
                        if(!saveAll())
                        {
                            toolkitShell->SetStatus(text.GetText("EditorUi.SaveFailed"));
                            return false;
                        }
                        std::string error;
                        if(!WriteBuildRestartRequest(requestPath,error))
                        {
                            toolkitShell->SetStatus(error);
                            return false;
                        }
                        toolkitShell->SetStatus(text.GetText("EditorUi.BuildRestart"));
                        Log(LogLevel::Info,"EditorUi.ToolkitBuild: restart requested");
                        if(hostWindow)PostMessageW(hostWindow,WM_CLOSE,0,0);
                        return true;
                    });
                    commands.Register(EditorCommandId::Undo,[&session](const EditorCommand&){return session.Undo();});
                    commands.Register(EditorCommandId::Redo,[&session](const EditorCommand&){return session.Redo();});
                    commands.Register(EditorCommandId::Play,[&session,&toolkitGameplayLogged,&toolkitImportTask](const EditorCommand&){
                        if(session.IsPlaying()){session.StopPlay();Log(LogLevel::Info,"EditorUi.ToolkitPlay: stop");return true;}
                        if(toolkitImportTask.valid())return false;
                        std::string reason;
                        if(!session.ValidatePlay(reason)){Log(LogLevel::Warning,"EditorUi.ToolkitPlay: "+reason);return false;}
                        if(!session.StartPlay()){Log(LogLevel::Warning,"EditorUi.ToolkitPlay: "+session.LastError());return false;}
                        session.SetPlayPaused(false);toolkitGameplayLogged=false;
                        Log(LogLevel::Info,"EditorUi.ToolkitPlay: start");return true;
                    });
                    commands.Register(EditorCommandId::Pause,[&session](const EditorCommand&){
                        if(!session.IsPlaying())return false;
                        const bool paused=session.PlayPaused();
                        if(!session.SetPlayPaused(!paused))return false;
                        Log(LogLevel::Info,paused?"EditorUi.ToolkitPlay: resume":"EditorUi.ToolkitPlay: pause");
                        return true;
                    });
                    commands.Register(EditorCommandId::Step,[&session](const EditorCommand&){
                        if(!session.IsPlaying()||!session.PlayPaused())return false;
                        session.RequestPlayStep();return true;
                    });
                    commands.Register(EditorCommandId::Stop,[&session](const EditorCommand&){
                        if(!session.IsPlaying())return false;
                        session.StopPlay();Log(LogLevel::Info,"EditorUi.ToolkitPlay: stop");return true;
                    });
                    commands.Register(EditorCommandId::Delete,[&session](const EditorCommand&){
                        return session.Selected()&&session.Delete(session.Selected());
                    });
                    commands.Register(EditorCommandId::Duplicate,[&session](const EditorCommand&){
                        return session.Selected()&&session.Duplicate(session.Selected())!=0;
                    });
                    commands.Register(EditorCommandId::Import,[toolkitImport](const EditorCommand&){return toolkitImport();});
                    commands.Register(EditorCommandId::Rename,[&toolkitShell](const EditorCommand&){
                        return toolkitShell&&toolkitShell->BeginRename();
                    });
                    commands.Register(EditorCommandId::TogglePanel,[&toolkitShell](const EditorCommand& command){
                        se::ui::editor::EditorPanelId panel=se::ui::editor::EditorPanelId::None;
                        if(!se::ui::editor::EditorPanelFromName(command.argument,panel))return false;
                        Log(LogLevel::Info,"EditorUi.ToolkitPanel: "+command.argument);
                        toolkitShell->TogglePanel(panel);return true;
                    });
                    commands.Register(EditorCommandId::FocusViewport,[&toolkitShell](const EditorCommand&){
                        toolkitShell->FocusPanel(EditorPanelId::Viewport);return true;
                    });
                    commands.Register(EditorCommandId::OpenSettings,[&toolkitShell](const EditorCommand&){
                        Log(LogLevel::Info,"EditorUi.ToolkitPanel: project-settings");
                        toolkitShell->TogglePanel(EditorPanelId::ProjectSettings);return true;
                    });
                }
                {
                    std::string settingsError;
                    if(!ProjectSettingsService::ReadProject(root/"project.json",toolkitProjectSettings,settingsError))
                        Log(LogLevel::Warning,"EditorUi.ToolkitProjectSettings: "+settingsError);
                    if(!ProjectSettingsService::ReadBuild(root/"build.json",toolkitBuildSettings,settingsError))
                        Log(LogLevel::Warning,"EditorUi.ToolkitBuildSettings: "+settingsError);
                    const auto projectRows=[&toolkitProjectSettings,&text](){
                        std::vector<ui::editor::EditorProperty> rows;
                        rows.push_back({"name",text.GetText("EditorUi.Name"),ui::editor::EditorPropertyType::String,toolkitProjectSettings.name,false});
                        rows.push_back({"contentRoot",text.GetText("EditorUi.ContentRoot"),ui::editor::EditorPropertyType::String,toolkitProjectSettings.contentRoot,false});
                        rows.push_back({"entryScene",text.GetText("EditorUi.EntryScene"),ui::editor::EditorPropertyType::String,toolkitProjectSettings.entryScene,false});
                        rows.push_back({"assets",text.GetText("EditorUi.Assets"),ui::editor::EditorPropertyType::String,
                            std::to_string(toolkitProjectSettings.assets.size())+" "+text.GetText("EditorUi.Registered"),true});
                        return rows;
                    };
                    const auto buildRows=[&toolkitBuildSettings,&toolkitExportName,&text](){
                        std::vector<ui::editor::EditorProperty> rows;
                        rows.push_back({"exportName",text.GetText("EditorUi.ExportName"),ui::editor::EditorPropertyType::String,toolkitExportName,false});
                        rows.push_back({"scriptsProject",text.GetText("EditorUi.ScriptsProject"),ui::editor::EditorPropertyType::String,toolkitBuildSettings.scriptsProject,false});
                        rows.push_back({"scriptType",text.GetText("EditorUi.ScriptType"),ui::editor::EditorPropertyType::String,toolkitBuildSettings.scriptType,false});
                        rows.push_back({"bindings",text.GetText("EditorUi.Bindings"),ui::editor::EditorPropertyType::String,toolkitBuildSettings.bindings,false});
                        rows.push_back({"entryUi",text.GetText("EditorUi.EntryUi"),ui::editor::EditorPropertyType::String,toolkitBuildSettings.entryUi,false});
                        return rows;
                    };
                    toolkitShell->SetFormProperties(EditorPanelId::ProjectSettings,projectRows());
                    toolkitShell->SetFormProperties(EditorPanelId::BuildExport,buildRows());
                    toolkitShell->SetFormHandlers(EditorPanelId::ProjectSettings,
                        [&toolkitProjectSettings,&toolkitProjectDirty](std::string key,std::string value){
                            if(key=="name")toolkitProjectSettings.name=value;
                            else if(key=="contentRoot")toolkitProjectSettings.contentRoot=value;
                            else if(key=="entryScene")toolkitProjectSettings.entryScene=value;
                            else return false;
                            toolkitProjectDirty=true;
                            return true;
                        },
                        [&toolkitProjectSettings,&toolkitShell,&root,&toolkitProjectDirty,&text](){
                            std::string error;
                            if(!ProjectSettingsService::WriteProject(root/"project.json",toolkitProjectSettings,error))
                            {
                                toolkitShell->SetFormStatus(EditorPanelId::ProjectSettings,error);
                                return false;
                            }
                            toolkitProjectDirty=false;
                            toolkitShell->SetFormStatus(EditorPanelId::ProjectSettings,text.GetText("EditorUi.Saved"));
                            Log(LogLevel::Info,"EditorUi.ToolkitProjectSettings: saved");
                            return true;
                        });
                    toolkitShell->SetFormHandlers(EditorPanelId::BuildExport,
                        [&toolkitBuildSettings,&toolkitBuildDirty,&toolkitExportName](std::string key,std::string value){
                            if(key=="exportName")
                            {
                                if(value.empty()||value.size()>128)return false;
                                toolkitExportName=std::move(value);
                                return true;
                            }
                            if(key=="scriptsProject")toolkitBuildSettings.scriptsProject=value;
                            else if(key=="scriptType")toolkitBuildSettings.scriptType=value;
                            else if(key=="bindings")toolkitBuildSettings.bindings=value;
                            else if(key=="entryUi")toolkitBuildSettings.entryUi=value;
                            else return false;
                            toolkitBuildDirty=true;
                            return true;
                        },
                        [&toolkitBuildSettings,&toolkitShell,&root,&toolkitBuildDirty,&text](){
                            std::string error;
                            if(!ProjectSettingsService::WriteBuild(root/"build.json",toolkitBuildSettings,error))
                            {
                                toolkitShell->SetFormStatus(EditorPanelId::BuildExport,error);
                                return false;
                            }
                            toolkitBuildDirty=false;
                            toolkitShell->SetFormStatus(EditorPanelId::BuildExport,text.GetText("EditorUi.Saved"));
                            Log(LogLevel::Info,"EditorUi.ToolkitBuildSettings: saved");
                            return true;
                        });
                    toolkitShell->SetFormAction(EditorPanelId::BuildExport,text.GetText("EditorUi.Export"),[&toolkitExportAction](){
                        return toolkitExportAction ? toolkitExportAction() : false;
                    });
                    toolkitShell->SetFormStatus(EditorPanelId::Material,text.GetText("EditorUi.SelectMaterial"));
                    toolkitShell->SetFormHandlers(EditorPanelId::Material,
                        [&frontend](std::string key,std::string value){
                            const PropertyDescriptor* descriptor=FindMaterialProperty(key);
                            if(!descriptor)return false;
                            PropertyValue parsed;
                            if(!ParsePropertyValue(*descriptor,value,parsed))return false;
                            const bool applied=frontend.SetMaterialPropertyForToolkit(key,parsed);
                            if(applied)Log(LogLevel::Info,"EditorUi.ToolkitMaterial: property "+key);
                            return applied;
                        },
                        [&frontend,&assets,&toolkitShell,&text](){
                            std::string error;
                            if(!frontend.SaveMaterialForToolkit(assets,error))
                            {
                                toolkitShell->SetFormStatus(EditorPanelId::Material,
                                    error.empty()?"Save material failed":error);
                                return false;
                            }
                            toolkitShell->SetFormProperties(EditorPanelId::Material,MaterialRows(frontend,text));
                            toolkitShell->SetFormStatus(EditorPanelId::Material,text.GetText("EditorUi.Saved"));
                            Log(LogLevel::Info,"EditorUi.ToolkitMaterial: saved");
                            return true;
                        });
                    const auto preferencesRows=[&preferences,&text](){
                        std::vector<ui::editor::EditorProperty> rows;
                        rows.push_back({"language",text.GetText("EditorUi.Language"),ui::editor::EditorPropertyType::Combo,preferences.language,false,{"zh-CN","en-US"}});
                        rows.push_back({"uiScale",text.GetText("EditorUi.UiScale"),ui::editor::EditorPropertyType::Number,std::to_string(preferences.uiScale),false});
                        rows.push_back({"externalEditor",text.GetText("EditorUi.ExternalEditor"),ui::editor::EditorPropertyType::String,preferences.externalEditor,false});
                        rows.push_back({"colorLut",text.GetText("EditorUi.ColorLut"),ui::editor::EditorPropertyType::String,preferences.colorLut,false});
                        rows.push_back({"restoreLastProject",text.GetText("EditorUi.RestoreLastProject"),ui::editor::EditorPropertyType::Boolean,preferences.restoreLastProject?"true":"false",false});
                        rows.push_back({"recentProjects",text.GetText("EditorUi.RecentProjects"),ui::editor::EditorPropertyType::String,
                            std::to_string(preferences.recentProjects.size()),true});
                        return rows;
                    };
                    toolkitShell->SetFormProperties(EditorPanelId::Preferences,preferencesRows());
                    toolkitShell->SetFormHandlers(EditorPanelId::Preferences,
                        [&preferences](std::string key,std::string value){
                            if(key=="language")preferences.language=value;
                            else if(key=="uiScale")
                            {
                                try{const float scale=std::stof(value);if(!std::isfinite(scale)||scale<0.5f||scale>2.0f)return false;preferences.uiScale=scale;}
                                catch(...){return false;}
                            }
                            else if(key=="externalEditor")preferences.externalEditor=value;
                            else if(key=="colorLut")preferences.colorLut=value;
                            else if(key=="restoreLastProject")preferences.restoreLastProject=(value=="true");
                            else return false;
                            return true;
                        },
                        [&preferences,&toolkitShell,&text](){
                            std::string error;
                            if(!EditorSessionStore::SavePreferences(EditorSessionStore::UserDataDirectory(),preferences,error))
                            {
                                toolkitShell->SetFormStatus(EditorPanelId::Preferences,error.empty()?text.GetText("EditorUi.SaveFailed"):error);
                                return false;
                            }
                            toolkitShell->SetFormStatus(EditorPanelId::Preferences,
                                text.GetText("EditorUi.Saved")+" ("+text.GetText("EditorUi.RestartToApply")+")");
                            Log(LogLevel::Info,"EditorUi.ToolkitPreferences: saved");
                            return true;
                        });
                    toolkitShell->SetPanelText(EditorPanelId::About,text.GetText("EditorUi.AboutText"));
                    toolkitShell->SetPanelText(EditorPanelId::Diagnostics,
                        text.GetText("EditorUi.DiagnosticsTitle")+"\n"+
                        "Project: "+asset_io::Utf8(root)+"\n"+
                        "Scene: "+(session.Path().empty()?std::string("-"):asset_io::Utf8(session.Path()))+"\n"+
                        "DPI: "+std::to_string(GetDpiForWindow(window))+"\n"+
                        "UI scale: "+std::to_string(preferences.uiScale)+"\n"+
                        "UI: se::ui toolkit");
                }
                {
                    wchar_t openPanel[64]{};
                    if(GetEnvironmentVariableW(L"SELFENGINE_EDITOR_OPEN_PANEL",openPanel,64)>0)
                    {
                        const std::string name=asset_io::Utf8(openPanel);
                        se::ui::editor::EditorPanelId panel=se::ui::editor::EditorPanelId::None;
                        if(se::ui::editor::EditorPanelFromName(name,panel))toolkitShell->FocusPanel(panel);
                    }
                    wchar_t openAsset[512]{};
                    if(GetEnvironmentVariableW(L"SELFENGINE_EDITOR_OPEN_ASSET",openAsset,512)>0)
                        toolkitPendingAsset=asset_io::Utf8(openAsset);
                    wchar_t autoExport[8]{};
                    toolkitAutoExport=GetEnvironmentVariableW(L"SELFENGINE_EDITOR_AUTO_EXPORT",autoExport,8)>0;
                    wchar_t autoFlow[8]{};
                    toolkitAutoExportFlow=GetEnvironmentVariableW(L"SELFENGINE_EDITOR_AUTO_EXPORT_FLOW",autoFlow,8)>0;
                    wchar_t autoCancel[16]{};
                    if(GetEnvironmentVariableW(L"SELFENGINE_EDITOR_AUTO_EXPORT_CANCEL_MS",autoCancel,16)>0)
                        toolkitAutoCancelMs=std::atoi(asset_io::Utf8(autoCancel).c_str());
                }
                const UINT dpi=GetDpiForWindow(window);
                float uiScale=dpi?static_cast<float>(dpi)/96.0f:1.0f;
                wchar_t dpiOverride[16]{};
                if(GetEnvironmentVariableW(L"SELFENGINE_TEST_DPI_SCALE",dpiOverride,16)>0)
                {
                    const float parsed=static_cast<float>(std::atof(asset_io::Utf8(dpiOverride).c_str()));
                    if(parsed>0.5f&&parsed<4.0f){uiScale=parsed;toolkitDpiOverride=true;}
                }
                toolkitInput=std::make_unique<EditorInputAdapter>(uiScale);
                toolkitInput->SetWindow(window);
                floatingWindows=std::make_unique<EditorFloatingWindows>(window,*toolkitShell,text,resourceDirectory,
                    [&](HWND target,UINT message,WPARAM w,LPARAM l){return services.messageHandler(target,message,w,l);},!hidden);
                {
                    std::string recoveryError;
                    auto recoveryGroups=EditorSessionStore::ListRecoveryGroups(crashGuard.directory,root,recoveryError);
                    if(!recoveryGroups.empty())
                    {
                        auto groups=std::make_shared<std::vector<RecoveryGroup>>(std::move(recoveryGroups));
                        const auto directory=crashGuard.directory;
                        const auto discardAll=[groups,directory](){
                            for(const RecoveryGroup& group:*groups)
                            {
                                std::string error;
                                EditorSessionStore::DiscardRecoveryGroup(directory,group.id,error);
                            }
                        };
                        const auto recover=[&session,&toolkitShell,&text,&toolkitExternalChange,groups,directory,discardAll](){
                            std::string recoveryResult;
                            const bool opened=ApplyRecoveryScene(groups->front(),directory,session,recoveryResult);
                            if(opened)toolkitExternalChange.Reset(session.Path());
                            discardAll();
                            toolkitShell->SetStatus(opened?text.GetText("EditorUi.RecoveryOpened"):
                                text.GetText("EditorUi.RecoveryFailed"));
                            Log(LogLevel::Info,opened?"EditorUi.ToolkitRecovery: opened":"EditorUi.ToolkitRecovery: failed");
                        };
                        toolkitShell->ShowConfirm(text.GetText("EditorUi.RecoveryTitle"),
                            text.GetText("EditorUi.RecoveryMessage")+" ("+std::to_string(groups->size())+")",
                            text.GetText("EditorUi.Recover"),text.GetText("EditorUi.DiscardChanges"),
                            text.GetText("EditorUi.Cancel"),recover,[discardAll](){discardAll();});
                    }
                }
                if(toolkitAutoExport&&toolkitExportAction)toolkitAutoExportFn=toolkitExportAction;
                Log(LogLevel::Info,"EditorUi.ToolkitHostReady");
                return true;
            }
            platform=std::make_unique<WindowsEditorInput>(window,frontend);
            if(!platform->Ready()){Log(LogLevel::Error,text.GetText(platform->LastError()));return false;}
            Log(LogLevel::Info,text.GetText("EditorUi.HostReady"));return true;
        };
        services.windowClosing=[&]{
            playInput->Deactivate(hostWindow);
            if(toolkit){Log(LogLevel::Info,"EditorUi.ToolkitClosing");viewportController.Blur(session);floatingWindows.reset();toolkitInput.reset();toolkitShell.reset();toolkitText.reset();toolkitFontPixels.clear();}
            else{frontend.CancelInteractions(session,&ui);platform.reset();}
            hostWindow=nullptr;
        };
        services.messageHandler=[&](HWND window,UINT message,WPARAM wParam,LPARAM lParam){
            if(toolkit)
            {
                auto* windowInput=window==hostWindow?toolkitInput.get():(floatingWindows?floatingWindows->Input(window):nullptr);
                if(message==WM_SYSKEYDOWN&&wParam==VK_F4)return std::optional<LRESULT>{};
                if(message==WM_CLOSE)
                {
                    // 自动化/隐藏运行没有可交互的模态，直接按宿主退出；交互窗口才走关闭保护。
                    if(hidden||maximumFrames>0)return std::optional<LRESULT>{};
                    if(!toolkitShell||toolkitShell->CloseApproved())return std::optional<LRESULT>{};
                    toolkitShell->SetDirty(session.Dirty()||frontend.MaterialDirtyForToolkit()||toolkitProjectDirty||toolkitBuildDirty||toolkitUiDirty);
                    if(toolkitShell->RequestClose())return std::optional<LRESULT>{0};
                    return std::optional<LRESULT>{};
                }
                if(message==WM_DPICHANGED)
                {
                    const UINT dpi=HIWORD(wParam);
                    Log(LogLevel::Info,"EditorUi.ToolkitDpi: "+std::to_string(dpi));
                    if(windowInput&&dpi)windowInput->SetDpiScale(static_cast<float>(dpi)/96.0f);
                    if(lParam)
                    {
                        const auto& bounds=*reinterpret_cast<const RECT*>(lParam);
                        SetWindowPos(window,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,
                            bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);
                    }
                    return std::optional<LRESULT>{0};
                }
                if(message==WM_MOUSEWHEEL)
                {
                    POINT point{static_cast<short>(LOWORD(lParam)),static_cast<short>(HIWORD(lParam))};
                    ScreenToClient(window,&point);
                    lParam=MAKELPARAM(point.x,point.y);
                }
                HWND targetWindow=window;
                if(floatingWindows)
                {
                    targetWindow=floatingWindows->RoutePointer(window,message,lParam);
                    floatingWindows->SelectSurface(targetWindow);
                    if(targetWindow&&targetWindow!=window)
                        windowInput=targetWindow==hostWindow?toolkitInput.get():floatingWindows->Input(targetWindow);
                }
                ui::UiInputEvent event;
                if(windowInput&&toolkitShell)windowInput->SetTextInputActive(toolkitShell->TextFieldFocused());
                if(windowInput&&windowInput->Translate(message,wParam,lParam,event))
                {
                    if(!toolkitShell)return std::optional<LRESULT>{};
                    const bool lostFocus=event.type==ui::UiEventType::FocusLost;
                    const bool lostCapture=event.type==ui::UiEventType::PointerCaptureLost;
                    if(lostFocus||lostCapture)
                    {
                        // 主动松开捕获不等于失焦；保留视口焦点，随后 QWER 仍应切换工具。
                        if(lostFocus||viewportController.Captured())viewportController.Blur(session);
                        auto& dispatcher=toolkitShell->Dispatcher();
                        event.target=dispatcher.PointerCapture();
                        if(!event.target.IsValid())event.target=dispatcher.Focus();
                        dispatcher.Dispatch(event);
                        dispatcher.ReleasePointerCapture();
                        if(lostFocus)dispatcher.ClearFocus();
                        if(GetCapture()==window)ReleaseCapture();
                        return std::optional<LRESULT>{};
                    }
                    const bool modal=toolkitShell->CloseModalOpen()||toolkitShell->ConfirmOpen()||toolkitShell->MenuOpen();
                    const auto hit=ui::UiHitTest(toolkitShell->Document(),toolkitShell->Layout()).Hit(
                        toolkitShell->CurrentInputRoot(),event.x,event.y).target;
                    const bool onViewport=hit==toolkitShell->ViewportPanel();
                    const bool key=event.type==ui::UiEventType::KeyDown||event.type==ui::UiEventType::KeyUp;
                    const bool blocked=modal||(key&&toolkitShell->TextFieldFocused());
                    if((key||event.type==ui::UiEventType::TextInput)&&GetEnvironmentVariableW(L"SELFENGINE_EDITOR_TRACE_VIEWPORT",nullptr,0))
                    {
                        Log(LogLevel::Info,text.GetText("EditorUi.ViewportInputTrace")+": message="+std::to_string(message)
                            +", raw="+std::to_string(wParam)+", key="+std::to_string(event.key)+", char="+std::to_string(event.codepoint)
                            +", ctrl="+std::to_string(event.control)+", alt="+std::to_string(event.alt)+", blocked="+std::to_string(blocked));
                        std::cout<<std::flush;
                    }
                    if(blocked)viewportController.Blur(session);
                    if(event.type==ui::UiEventType::PointerDown&&!viewportController.Captured())
                    {
                        if(!onViewport)viewportController.Blur(session);
                        else if(!modal)toolkitShell->Dispatcher().ClearFocus();
                    }
                    bool handled=false;
                    const bool captured=viewportController.Captured();
                    if(!blocked&&(captured||key))handled=viewportController.Handle(event,session,assets,false);
                    if(!handled&&!captured&&event.type==ui::UiEventType::KeyDown)
                        handled=toolkitShell->RouteShortcut(event,blocked);
                    if(!handled&&!captured)
                    {
                        toolkitShell->Dispatcher().Dispatch(event);
                        if(!blocked&&!event.handled)handled=viewportController.Handle(event,session,assets,onViewport);
                    }
                    if(event.type==ui::UiEventType::PointerDown&&GetEnvironmentVariableW(L"SELFENGINE_EDITOR_TRACE_VIEWPORT",nullptr,0))
                    {
                        const auto hitRect=toolkitShell->Layout().Rect(hit);
                        Log(LogLevel::Info,text.GetText("EditorUi.ViewportInputTrace")+": x="+std::to_string(event.x)+", y="+std::to_string(event.y)
                            +", viewport="+std::to_string(onViewport)+", blocked="+std::to_string(blocked)+", ui="+std::to_string(event.handled)
                            +", hit="+std::to_string(hitRect.x)+","+std::to_string(hitRect.y)+","+std::to_string(hitRect.width)+","+std::to_string(hitRect.height)
                            +", selected="+std::to_string(session.Selected())+", tool="+std::to_string(viewportController.Tool()));
                    }
                    const bool pointerCaptured=viewportController.Captured() ||
                        toolkitShell->Dispatcher().PointerCapture().IsValid();
                    if(pointerCaptured&&GetCapture()!=window)SetCapture(window);
                    else if(!pointerCaptured&&GetCapture()==window)ReleaseCapture();
                    windowInput->SetTextInputActive(toolkitShell->TextFieldFocused());
                    return std::optional<LRESULT>{0};
                }
                return std::optional<LRESULT>{};
            }
            if(message==WM_CLOSE&&!frontend.CloseApproved()){frontend.RequestClose();return std::optional<LRESULT>{0};}
            const auto routed=playInput->Process(window,message,wParam,lParam);
            const auto editorResult=routed.forwardEditor&&platform?platform->Process(window,message,wParam,lParam):std::optional<LRESULT>{};
            return routed.handled?routed.handled:editorResult;
        };
        services.captureKeyboard=[&]{if(toolkit)return false;return frontend.FileDialogActive()||(session.IsPlaying()?playInput->KeyboardConsumed():platform&&platform->WantsKeyboard());};
        services.capturePointer=[&]{if(toolkit)return false;return frontend.FileDialogActive()||(session.IsPlaying()?playInput->PointerConsumed():platform&&platform->WantsMouse());};
        services.input=[&](const InputState& input,bool keyboard,bool pointer){
            if(toolkit)
            {
                if(session.IsPlaying()&&!toolkitGameplayLogged)
                {
                    bool hasGameplayInput=!input.pointerEvents.empty();
                    for(bool down:input.keysDown)hasGameplayInput=hasGameplayInput||down;
                    if(hasGameplayInput){toolkitGameplayLogged=true;Log(LogLevel::Info,"EditorUi.ToolkitGameplayInput");}
                }
                if(hooks&&hooks->input&&session.IsPlaying())
                {
                    float dpi=toolkitInput?toolkitInput->DpiScale():1.0f;
                    const auto* floatingInput=floatingWindows?floatingWindows->GameplayInput(dpi):nullptr;
                    InputState gameplay=floatingInput?*floatingInput:input;
                    const ui::editor::ViewportRectF viewportMath{toolkitViewportRect.x,toolkitViewportRect.y,toolkitViewportRect.width,toolkitViewportRect.height};
                    float mouseX=static_cast<float>(gameplay.mouseX),mouseY=static_cast<float>(gameplay.mouseY);
                    ui::editor::ViewportOffsetPlayInput(viewportMath,dpi,mouseX,mouseY);
                    gameplay.mouseX=static_cast<int>(std::lround(mouseX));gameplay.mouseY=static_cast<int>(std::lround(mouseY));
                    for(auto& event:gameplay.pointerEvents){float ex=static_cast<float>(event.x),ey=static_cast<float>(event.y);ui::editor::ViewportOffsetPlayInput(viewportMath,dpi,ex,ey);event.x=static_cast<int>(std::lround(ex));event.y=static_cast<int>(std::lround(ey));}
                    hooks->input(gameplay,keyboard,pointer);
                }
                playInput->EndFrame();
                return;
            }
            if(hooks&&hooks->input&&session.IsPlaying()&&!frontend.FileDialogActive())hooks->input(playInput->GameplayInput(input),keyboard,pointer);
            playInput->EndFrame();
        };
        services.runEnvironmentFrame=[&](double seconds){
            if(toolkit)
            {
                viewportController.Update(seconds,session,assets);
                const bool uiCaptured=toolkitShell&&toolkitShell->Dispatcher().PointerCapture().IsValid();
                if(!viewportController.Captured()&&!uiCaptured&&
                    (GetCapture()==hostWindow||(floatingWindows&&floatingWindows->Owns(GetCapture()))))ReleaseCapture();
            }
            if(frontend.TimeOfDayPreview().Ready()&&!frontend.TimeOfDayPreview().Advance(seconds))return false;
            if(hooks&&hooks->frame&&!hooks->frame(session,seconds))return false;
            ui.Update(seconds);
            if(frontend.FileDialogActive())return true;
            if(session.IsPlaying()&&playUi)
            {
                // 外部运行宿主统一推进脚本/物理/UI，避免同一 Play UI 计时两次。
                if(session.ConsumePlayStep())
                {
                    if(hooks&&hooks->singleStep)
                    {if(!hooks->singleStep(*session.PlayWorld(),*playUi))return false;}
                    else
                    {playUi->Update(session.PlayFixedStep());if(!session.StepPlayFrame())return false;}
                }
                else if(hooks&&hooks->update){if(!hooks->update(*session.PlayWorld(),*playUi,seconds))return false;}
                else {if(!session.PlayPaused())playUi->Update(seconds);if(!session.PlayEnvironment()->RunFrame(seconds))return false;}
                session.CompletePlayFrame();
            }
            return toolkit || (platform&&platform->Ready());
        };
        services.extract=[&](FrameData& frame){
            if(toolkit)
            {
                if(!toolkitShell||!hostWindow)throw std::runtime_error("EditorUi.ToolkitNotReady");
                if(hooks&&hooks->operationFrame)hooks->operationFrame(frontend,session);
                if(toolkitImportTask.valid()&&toolkitImportTask.wait_for(std::chrono::seconds(0))==std::future_status::ready)
                {
                    ToolkitImportResult result;
                    try{result=toolkitImportTask.get();}
                    catch(const std::exception& failure){result.key="Editor.AssetImportFailed";result.detail=failure.what();}
                    if(result.success)
                    {
                        std::vector<AssetDiagnostic> failures;
                        if(!assets.Scan(failures))
                        {
                            result.success=false;result.key="Assets.ScanFailed";
                            if(!failures.empty()){result.key=failures.front().key;result.detail=failures.front().detail;}
                        }
                        else
                        {
                            toolkitPendingAsset=asset_io::Utf8(result.primary);
                            std::vector<EditorAssetIssue> issues;
                            if(!EditorAssetActions::SyncProjectAssets(root,assets,issues))
                                for(const auto& issue:issues)
                                {
                                    const auto message=text.GetText(issue.key)+": "+issue.detail;
                                    toolkitShell->AppendConsoleLine(message);Log(LogLevel::Warning,message);
                                }
                        }
                    }
                    if(result.key.empty())result.key=result.success?"Editor.AssetImportSucceeded":"Editor.AssetImportFailed";
                    toolkitImportStatus=text.GetText(result.key)+(result.detail.empty()?std::string{}:": "+result.detail);
                    toolkitShell->SetAssetsPreviewText(toolkitImportStatus);
                    toolkitShell->AppendConsoleLine(toolkitImportStatus);
                    Log(result.success?LogLevel::Info:LogLevel::Warning,toolkitImportStatus);
                    toolkitShell->FocusPanel(EditorPanelId::Assets);
                }
#if defined(FLARE_LOCAL_VALIDATION)
                if(smoke.Enabled())
                {
                    // 两套壳共用同一个验收驱动；toolkit 必须执行自己的生产命令总线。
                    smoke.OperationFrame(frontend,session,[&](std::string_view id){
                        std::optional<EditorCommandId> command;
                        if(id=="editor.play")
                        {
                            std::string reason;
                            if(!session.ValidatePlay(reason))return EditorCommandOutcome::Rejected(reason);
                            command=EditorCommandId::Play;
                        }
                        else if(id=="editor.pause"||id=="editor.resume")
                        {
                            if(!session.IsPlaying())return EditorCommandOutcome::Rejected("Editor.PlaybackInvalidState");
                            if(session.PlayPaused()==(id=="editor.pause"))return EditorCommandOutcome::Succeeded();
                            command=EditorCommandId::Pause;
                        }
                        else if(id=="editor.step")command=EditorCommandId::Step;
                        else if(id=="editor.stop")command=EditorCommandId::Stop;
                        else if(id=="editor.save-all")command=EditorCommandId::SaveAll;
                        else if(id=="editor.compile")command=EditorCommandId::Build;
                        if(!command)return EditorCommandOutcome::Rejected("Editor.CommandUnknown",std::string(id));
                        return toolkitShell->Commands().Execute({*command,{}})?EditorCommandOutcome::Succeeded():
                            EditorCommandOutcome::Failed("Editor.PlaybackInvalidState");
                    },[&]{PostMessageW(hostWindow,WM_CLOSE,0,0);});
                }
#endif
                static bool toolkitExtractLogged=false;
                if(!toolkitExtractLogged)
                {
                    toolkitExtractLogged=true;
                    Log(LogLevel::Info,"EditorUi.ToolkitExtract");
                }
                if(toolkitAutoExportFn&&!toolkitAutoExportStarted)
                {
                    toolkitAutoExportStarted=true;
                    toolkitAutoExportStart=GetTickCount64();
                    toolkitAutoExportFn();
                }
                RECT client{};
                GetClientRect(hostWindow,&client);
                const float width=static_cast<float>(client.right-client.left);
                const float height=static_cast<float>(client.bottom-client.top);
                const float dpi=toolkitInput?toolkitInput->DpiScale():1.0f;
                toolkitShell->SetDpiScale(dpi);
                toolkitShell->Resize(width,height);
                if(floatingWindows&&!floatingWindows->Synchronize())throw std::runtime_error(text.GetText("EditorUi.FloatingWindowFailed"));
                float framebufferScaleX=dpi,framebufferScaleY=dpi;
                if(toolkitDpiOverride&&toolkitShell->Width()>0.0f&&toolkitShell->Height()>0.0f)
                {
                    framebufferScaleX=width/toolkitShell->Width();
                    framebufferScaleY=height/toolkitShell->Height();
                }
                toolkitText->SetRasterScale(framebufferScaleY);
                toolkitShell->Frame();
                {
                    std::vector<ui::editor::EditorHierarchyNode> hierarchy;
                    const World& editWorld=session.EditWorld();
                    for(const EditorHierarchyItem& item:BuildEditorHierarchy(editWorld))
                    {
                        ui::editor::EditorHierarchyNode node;
                        node.id=item.id;node.parent=item.parent;node.depth=item.depth;
                        node.hasChildren=item.hasChildren;node.expanded=true;
                        if(const EntityRecord* record=editWorld.Get(editWorld.FindByStableId(item.id)))node.label=record->name;
                        else node.label="Entity "+std::to_string(item.id);
                        hierarchy.push_back(std::move(node));
                    }
                    toolkitShell->SetHierarchy(std::move(hierarchy));
                    std::vector<ui::editor::EditorAssetEntry> entries;
                    for(const AssetId& id:assets.ListIds())
                    {
                        const std::filesystem::path resolved=assets.Resolve(id);
                        const std::string extension=resolved.extension().string();
                        std::string type="asset";
                        if(extension==".fbx"||extension==".gltf"||extension==".glb")type="model";
                        else if(extension==".png"||extension==".jpg"||extension==".jpeg"||extension==".tga")type="texture";
                        else if(extension==".hlsl"||extension==".hlsli")type="shader";
                        else if(extension==".json")type="data";
                        entries.push_back({asset_io::Utf8(resolved),type});
                    }
                    std::string pendingAssetPath;
                    if(!toolkitPendingAsset.empty())
                    {
                        for(const ui::editor::EditorAssetEntry& entry:entries)
                        {
                            if(entry.path.find(toolkitPendingAsset)!=std::string::npos)
                            {
                                pendingAssetPath=entry.path;
                                toolkitPendingAsset.clear();
                                break;
                            }
                        }
                    }
                    toolkitShell->SetAssets(std::move(entries));
                    if(!pendingAssetPath.empty())toolkitShell->SelectAsset(pendingAssetPath);
                    std::vector<ui::editor::EditorProperty> properties;
                    std::string inspectorLabel=text.GetText("EditorUi.NoSelection");
                    const StableEntityId selected=session.Selected();
                    if(selected&&editWorld.Get(editWorld.FindByStableId(selected)))
                    {
                        const EntityRecord* record=editWorld.Get(editWorld.FindByStableId(selected));
                        inspectorLabel=record->name;
                        properties.push_back({"name",text.GetText("EditorUi.Name"),ui::editor::EditorPropertyType::String,record->name,false});
                        properties.push_back({"stableId",text.GetText("EditorUi.StableId"),ui::editor::EditorPropertyType::String,std::to_string(selected),true});
                        if(const CameraComponent* camera=editWorld.GetCamera(editWorld.FindByStableId(selected)))
                        {
                            properties.push_back({"camera.fov",text.GetText("EditorUi.CameraFov"),ui::editor::EditorPropertyType::Number,std::to_string(camera->verticalFovRadians),false});
                        }
                        if(const LightComponent* light=editWorld.GetLight(editWorld.FindByStableId(selected)))
                        {
                            properties.push_back({"light.intensity",text.GetText("EditorUi.LightIntensity"),ui::editor::EditorPropertyType::Number,std::to_string(light->intensity),false});
                        }
                        if(const MeshComponent* mesh=editWorld.GetMesh(editWorld.FindByStableId(selected)))
                        {
                            properties.push_back({"mesh.asset",text.GetText("EditorUi.MeshAsset"),ui::editor::EditorPropertyType::Asset,mesh->meshAsset,false});
                            properties.push_back({"mesh.material",text.GetText("EditorUi.MaterialSlot"),ui::editor::EditorPropertyType::Asset,mesh->materials.empty()?std::string{}:mesh->materials.front(),false});
                        }
                        if(const Transform* transform=editWorld.GetTransform(editWorld.FindByStableId(selected)))
                        {
                            properties.push_back({"position.x",text.GetText("EditorUi.Position")+" X",ui::editor::EditorPropertyType::Number,std::to_string(transform->position.x),false});
                            properties.push_back({"position.y",text.GetText("EditorUi.Position")+" Y",ui::editor::EditorPropertyType::Number,std::to_string(transform->position.y),false});
                            properties.push_back({"position.z",text.GetText("EditorUi.Position")+" Z",ui::editor::EditorPropertyType::Number,std::to_string(transform->position.z),false});
                            properties.push_back({"scale.x",text.GetText("EditorUi.Scale")+" X",ui::editor::EditorPropertyType::Number,std::to_string(transform->scale.x),false});
                            properties.push_back({"scale.y",text.GetText("EditorUi.Scale")+" Y",ui::editor::EditorPropertyType::Number,std::to_string(transform->scale.y),false});
                            properties.push_back({"scale.z",text.GetText("EditorUi.Scale")+" Z",ui::editor::EditorPropertyType::Number,std::to_string(transform->scale.z),false});
                        }
                    }
                    const SceneEnvironmentSettings environment=editWorld.EnvironmentSettings();
                    properties.push_back({"environment.todProfile",text.GetText("EditorUi.TodProfile"),ui::editor::EditorPropertyType::Asset,environment.todProfile,false});
                    properties.push_back({"environment.preserveTime",text.GetText("EditorUi.TodPreserveTime"),ui::editor::EditorPropertyType::Boolean,environment.preserveTime?"true":"false",false});
                    if(properties.empty())properties.push_back({"selection",text.GetText("EditorUi.Selection"),ui::editor::EditorPropertyType::String,text.GetText("EditorUi.NoSelection"),true});
                        toolkitShell->SetInspector(selected,inspectorLabel,std::move(properties));
                }
                {
                    const ui::UiRect designerContent=toolkitShell->Layout().Rect(toolkitShell->UiDesignerContent());
                    if(designerContent.width>2.0f&&designerContent.height>2.0f)
                    {
                        frontend.UiCanvas().Prepare(ui,{designerContent.x,designerContent.y,designerContent.width,designerContent.height});
                        std::vector<ui::editor::EditorDesignerNode> designerNodes;
                        for(const UiVisual& visual:frontend.UiCanvas().Visuals(ui))
                        {
                            ui::editor::EditorDesignerNode node;
                            node.id=visual.id;
                            node.rect={visual.rectangle.x,visual.rectangle.y,visual.rectangle.width,visual.rectangle.height};
                            node.text=visual.text;
                            node.color=ui::UiColor{visual.color.r,visual.color.g,visual.color.b,visual.color.a};
                            node.selected=visual.id==frontend.SelectedUi();
                            node.visible=true;
                            designerNodes.push_back(std::move(node));
                        }
                        toolkitShell->SetDesignerNodes(std::move(designerNodes));
                    }
                }
                if(toolkitInput)
                {
                    const ui::UiElementHandle focus=toolkitShell->Dispatcher().Focus();
                    if(focus.IsValid())
                    {
                        const ui::UiRect rect=toolkitShell->Layout().Rect(focus);
                        const auto surface=toolkitShell->SurfaceForElement(focus);
                        auto* input=surface&&floatingWindows?floatingWindows->Input(floatingWindows->Window(surface)):toolkitInput.get();
                        if(input)input->SetImeCandidateAnchor({rect.x,rect.y,rect.width,rect.height});
                    }
                }
                toolkitShell->SetStatus(session.IsPlaying()
                    ?text.GetText(session.PlayPaused()?"EditorUi.Paused":"EditorUi.Running")
                    :!toolkitImportStatus.empty()?toolkitImportStatus
                    :text.GetText(viewportController.LastError().empty()?"EditorUi.Ready":viewportController.LastError()));
                if(toolkitExternalChange.Path()!=session.Path())toolkitExternalChange.Reset(session.Path());
                if(!toolkitShell->ConfirmOpen()&&toolkitExternalChange.Poll())
                {
                    const auto changedPath=toolkitExternalChange.Path();
                    toolkitExternalChange.Consume();
                    toolkitShell->ShowConfirm(text.GetText("EditorUi.ExternalChangedTitle"),
                        text.GetText("EditorUi.ExternalChangedMessage")+": "+asset_io::Utf8(changedPath),
                        text.GetText("EditorUi.Reload"),text.GetText("EditorUi.Keep"),
                        text.GetText("EditorUi.Cancel"),
                        [&session,&toolkitShell,&toolkitExternalChange,&text,changedPath](){
                            if(session.Dirty())
                            {
                                toolkitShell->SetStatus(text.GetText("EditorUi.SaveFirst"));
                                return;
                            }
                            if(session.Open(changedPath,true))
                            {
                                toolkitExternalChange.Reset(session.Path());
                                toolkitShell->SetStatus(text.GetText("EditorUi.Reloaded"));
                            }
                            else toolkitShell->SetStatus(session.LastError());
                        },
                        [&toolkitShell,&text](){toolkitShell->SetStatus(text.GetText("EditorUi.Keep"));});
                }
                toolkitShell->SetDirty(session.Dirty()||frontend.MaterialDirtyForToolkit()||toolkitProjectDirty||toolkitBuildDirty||toolkitUiDirty);
                frontend.PollExportJob(text);
                const bool exportBusy=frontend.ExportBusy();
                if(toolkitAutoCancelMs>0&&exportBusy&&!toolkitAutoCancelRequested&&
                    GetTickCount64()-toolkitAutoExportStart>=static_cast<std::uint64_t>(toolkitAutoCancelMs))
                {
                    frontend.RequestCancelExport();
                    toolkitAutoCancelRequested=true;
                    Log(LogLevel::Info,"EditorUi.ToolkitExport: auto-cancel");
                }
                if(exportBusy!=toolkitExportBusy)
                {
                    toolkitExportBusy=exportBusy;
                    toolkitShell->SetFormActionLabel(EditorPanelId::BuildExport,
                        exportBusy?text.GetText("EditorUi.Cancel"):text.GetText("EditorUi.Export"));
                }
                if(!exportBusy)
                {
                    const auto snapshot=frontend.ExportSnapshot();
                    if(snapshot.finished)
                    {
                        std::string exportStatus=snapshot.cancelled?text.GetText("EditorUi.ExportCancelled"):
                            snapshot.report.state==ExportOperationState::Succeeded?text.GetText("EditorUi.ExportSucceeded"):
                            text.GetText("EditorUi.ExportFailed");
                        if(!snapshot.cancelled&&snapshot.report.state!=ExportOperationState::Succeeded&&
                            !snapshot.report.diagnosticKey.empty())exportStatus+=": "+snapshot.report.diagnosticKey;
                        toolkitShell->SetFormStatus(EditorPanelId::BuildExport,exportStatus);
                        if(toolkitExportStarted&&!toolkitExportFinished)
                        {
                            toolkitExportFinished=true;
                            Log(LogLevel::Info,std::string("EditorUi.ToolkitExport: finished ")+
                                (snapshot.cancelled?"cancelled":snapshot.report.state==ExportOperationState::Succeeded?"succeeded":"failed")+
                                (snapshot.report.diagnosticKey.empty()?"":(" "+snapshot.report.diagnosticKey)));
                        }
                        if(toolkitAutoExportFlow&&toolkitAutoExportFlowPhase==0&&snapshot.cancelled&&snapshot.finished)
                        {
                            toolkitAutoExportFlowPhase=1;
                            toolkitAutoCancelMs=0;
                            toolkitAutoCancelRequested=false;
                            toolkitExportStarted=false;
                            toolkitExportFinished=false;
                            if(toolkitAutoExportFn)toolkitAutoExportFn();
                        }
                    }
                }
                if(toolkitShell->ConsumeCloseRequest())PostMessageW(hostWindow,WM_CLOSE,0,0);
                BuildFrameData(session.IsPlaying()?*session.PlayWorld():session.EditWorld(),session.IsPlaying()?playUi.get():nullptr,nullptr,frame);
                const ui::UiRect viewportRect=toolkitShell->Layout().Rect(toolkitShell->ViewportPanel());
                viewportController.Prepare(session,assets,viewportRect,++editorFrame);
                toolkitCamera=viewportController.Camera();toolkitViewportRect=viewportRect;
                toolkitShell->SetViewportState(viewportController.Tool(),viewportController.Local(),viewportController.Center(),
                    viewportController.Sky(),viewportController.Grid(),viewportController.Gizmos(),session.IsPlaying());
                toolkitShell->Frame();

                if(hooks&&hooks->toolkitDrawn)hooks->toolkitDrawn(*toolkitShell,session);
                ui::UiDisplayList display;
                ui::UiPaintStats paint;
                ui::UiPainter painter(toolkitShell->Document(),toolkitShell->Layout());
                if(!painter.Build(toolkitShell->Root(),display,paint))throw std::runtime_error("EditorUi.ToolkitPaintFailed");
                // 在场景图像之后插入辅助绘制，后续菜单/模态仍覆盖它。
                for(auto it=display.commands.begin();it!=display.commands.end();++it)
                    if(it->kind==ui::UiPaintCommandKind::Image&&it->image=="scene")
                    {display.commands.insert(it+1,viewportController.Overlay().commands.begin(),viewportController.Overlay().commands.end());break;}
                GuiGeometry geometry;
                ui::UiGeometryStats geometryStats;
                std::vector<std::string> errors;
                ui::UiGeometryOptions options;
                options.whiteTexture=3;
                options.fontPageBase=1;
                options.textPixelScale={framebufferScaleX,framebufferScaleY};
                options.whiteU=0.0f;options.whiteV=0.0f;options.whiteWidth=1.0f;options.whiteHeight=1.0f;
                ToolkitSceneImageProvider imageProvider;imageProvider.images=toolkitImageInfos;
                if(!ui::UiGeometryBuilder{}.Build(display,&toolkitShell->TextService(),&imageProvider,options,geometry,geometryStats,errors))
                    throw std::runtime_error("EditorUi.ToolkitGeometryFailed");
                if(!toolkitText||!toolkitText->FontAtlas(toolkitFontPixels,toolkitFontWidth,toolkitFontHeight,toolkitFontRevision))
                    throw std::runtime_error("EditorUi.ToolkitFontAtlasFailed");
                geometry.framebufferScale={framebufferScaleX,framebufferScaleY};
                frame.editorGui=std::move(geometry);
                frame.editorGuiEnabled=true;
                if(toolkitPreviewImage&&!toolkitPreviewImageKey.empty()&&toolkitPreviewToken!=0)
                {
                    frame.editorUiImages.push_back({toolkitPreviewToken,toolkitPreviewImage->width,
                        toolkitPreviewImage->height,toolkitPreviewImage->rgba8.data(),toolkitPreviewImage->rgba8.size()});
                }
                frame.editorDrawData=nullptr;
                frame.editorFontPixels=toolkitFontPixels.empty()?nullptr:toolkitFontPixels.data();
                frame.editorFontWidth=toolkitFontWidth;frame.editorFontHeight=toolkitFontHeight;frame.editorFontToken=1;
                frame.editorFontRevision=toolkitFontRevision;
                frame.assets=&assets;
                frame.sceneViewport={viewportRect.x,viewportRect.y,viewportRect.width,viewportRect.height};
                frame.editorCamera=viewportController.Camera();
                frame.editorSkyEnabled=viewportController.Sky();
                frame.editorGridEnabled=viewportController.Grid()&&!session.IsPlaying();
                frame.editorSceneLines=viewportController.SceneLines();
                frame.editorSelected=session.IsPlaying()?0:session.Selected();
                {
                    static float loggedViewport[4]={-1.0f,-1.0f,-1.0f,-1.0f};
                    if(loggedViewport[0]!=viewportRect.x||loggedViewport[1]!=viewportRect.y||loggedViewport[2]!=viewportRect.width||loggedViewport[3]!=viewportRect.height)
                    {
                        loggedViewport[0]=viewportRect.x;loggedViewport[1]=viewportRect.y;
                        loggedViewport[2]=viewportRect.width;loggedViewport[3]=viewportRect.height;
                        Log(LogLevel::Info,"EditorUi.ToolkitViewport: "+std::to_string(viewportRect.x)+","+std::to_string(viewportRect.y)+","+std::to_string(viewportRect.width)+","+std::to_string(viewportRect.height));
                    }
                }
                playInput->SetViewport(hostWindow,{viewportRect.x*dpi,viewportRect.y*dpi,viewportRect.width*dpi,viewportRect.height*dpi});
                if(floatingWindows&&hooks&&hooks->floatingDrawn)hooks->floatingDrawn(*floatingWindows,frame);
                if(floatingWindows&&!floatingWindows->Render(frame,imageProvider,viewportController.Overlay()))
                    throw std::runtime_error(text.GetText("EditorUi.FloatingRenderFailed"));
                return;
            }
            if(!platform||!platform->PrepareFrame())throw std::runtime_error("EditorUi.PlatformInitFailed");
            EditorInputFrame input;input.usePlatformInput=true;
            const auto renderView=renderer.QueryEditorView();
            EditorViewport viewport;viewport.hasCamera=renderView.hasCamera;viewport.viewProjection.values=renderView.viewProjection;
            viewport.colorTexture=static_cast<ImTextureID>(renderView.colorTextureToken);viewport.pick=renderView.pick;
            std::optional<CameraSnapshot> currentCamera;
            viewport.prepare=[&](EditorRect rect,bool canNavigate,EditorViewport& view){
                {
                    static float logged[4]={-1.0f,-1.0f,-1.0f,-1.0f};
                    if(logged[0]!=rect.x||logged[1]!=rect.y||logged[2]!=rect.width||logged[3]!=rect.height)
                    {
                        logged[0]=rect.x;logged[1]=rect.y;logged[2]=rect.width;logged[3]=rect.height;
                        Log(LogLevel::Info,"EditorUi.ImguiViewport: "+std::to_string(rect.x)+","+std::to_string(rect.y)+","+std::to_string(rect.width)+","+std::to_string(rect.height));
                    }
                }
                const CameraViewport area{0,0,rect.width,rect.height};
                if(!session.IsPlaying())
                {
                    auto& io=ImGui::GetIO();const auto mouse=io.MousePos;
                    const bool inside=mouse.x>=rect.x&&mouse.y>=rect.y&&mouse.x<rect.x+rect.width&&mouse.y<rect.y+rect.height;
                    const bool rightDown=ImGui::IsMouseDown(ImGuiMouseButton_Right);
                    if(!rightDown)navigationCancelled=false;
                    if(ImGui::IsKeyPressed(ImGuiKey_Escape,false)||io.AppFocusLost||(navigating&&(io.WantTextInput||ImGui::IsAnyItemActive())))navigationCancelled=true;
                    const bool navigate=canNavigate&&inside&&ImGui::IsWindowHovered()&&!io.AppFocusLost&&!io.WantTextInput&&!ImGui::IsAnyItemActive()&&rightDown&&!navigationCancelled;
                    if(navigate)
                    {
                        const auto down=[](ImGuiKey key){return ImGui::IsKeyDown(key)?1.0f:0.0f;};
                        editorCamera.Update({{down(ImGuiKey_D)-down(ImGuiKey_A),down(ImGuiKey_E)-down(ImGuiKey_Q),down(ImGuiKey_W)-down(ImGuiKey_S)},
                            navigating?Vec2{io.MouseDelta.x,io.MouseDelta.y}:Vec2{},io.KeyShift},io.DeltaTime);
                    }
                    navigating=navigate;
                    currentCamera=editorCamera.Snapshot(area,++editorFrame);
                    if(currentCamera)view.pick=[&,camera=*currentCamera](float x,float y){
                        const StableEntityId picked=PickEditorBounds(session.EditWorld(),assets,camera,x,y);
                        if(picked)Log(LogLevel::Info,"EditorUi.ImguiPick: "+std::to_string(picked));
                        return picked;
                    };
                }
                else
                {
                    navigating=false;navigationCancelled=false;
                    FrameData play;BuildFrameData(*session.PlayWorld(),nullptr,nullptr,play);
                    currentCamera=SelectCamera(play.cameras,area,++editorFrame).snapshot;
                }
                view.hasCamera=currentCamera.has_value();view.camera=currentCamera;if(currentCamera)view.viewProjection=currentCamera->viewProjection;
            };
            if(hooks&&hooks->operationFrame)hooks->operationFrame(frontend,session);
#if defined(FLARE_LOCAL_VALIDATION)
            smoke.OperationFrame(frontend,session);
#endif
            const auto result=frontend.Draw(session,assets,text,input,viewport,&ui);
            persistSession(false);
            if(frontend.CloseApproved())PostMessageW(hostWindow,WM_CLOSE,0,0);
            if(hooks&&hooks->drawn)hooks->drawn(frontend.Context(),result);
            playInput->SetViewport(hostWindow,{result.viewport.x,result.viewport.y,result.viewport.width,result.viewport.height});
            BuildFrameData(session.IsPlaying()?*session.PlayWorld():session.EditWorld(),session.IsPlaying()?playUi.get():nullptr,nullptr,frame);
            if(session.IsPlaying()&&session.PlayEnvironment()->TimeOfDay().Profile())frame.timeOfDay=session.PlayEnvironment()->Snapshot();
            if(!session.IsPlaying()&&frontend.TimeOfDayPreview().Ready())frame.timeOfDay=frontend.TimeOfDayPreview().Snapshot();
            if(result.uiCanvas)
            {
                frame.meshes.clear();frame.cameras.clear();frame.cameraBinding.reset();frame.lights.clear();frame.debugLines.clear();frame.ui=result.uiPreview;
                navigating=false;navigationCancelled=false;currentCamera.reset();
            }
            if(session.IsPlaying()&&playUi&&hooks&&hooks->extract)hooks->extract(*session.PlayWorld(),*playUi,frame);
            frame.assets=&assets;
            frame.sceneViewport={result.viewport.x,result.viewport.y,result.viewport.width,result.viewport.height};
            frame.editorDrawData=result.drawData;
            frame.editorFontPixels=fontPixels;
            frame.editorFontWidth=fontWidth;frame.editorFontHeight=fontHeight;frame.editorFontToken=1;
            if(!result.drawData||!CopyImGuiGeometry(*result.drawData,frame.editorGui))throw std::runtime_error("EditorUi.DrawDataInvalid");
            frame.editorGuiEnabled=true;
            // G15：编辑器 LUT 选择随帧到达 BasicLit 渲染器；路径不变时由渲染器自行热重载。
            frame.colorLutPath=frontend.ColorLutPath();
            if(!session.IsPlaying()&&!result.uiCanvas)frame.editorCamera=currentCamera;
            if(!session.IsPlaying()&&!result.uiCanvas)frame.materialPreview=frontend.CurrentMaterialPreview();
        };
        // RunSandboxApp 先 Shutdown renderer，再 windowClosing 释放平台，最后才返回此栈释放 context。
        WindowsEditorInput::EnableDpiAwareness();
        if(hooks&&hooks->startPlaying&&!session.StartPlay())return 1;
        const int result=RunSandboxApp(locale,renderer,!hidden,&services);
        if(toolkit)Log(LogLevel::Info,std::string("EditorUi.ToolkitResult: ")+std::to_string(result));
        if(toolkit)Log(LogLevel::Info,"EditorUi.ToolkitBeforePersist");
        session.StopPlay();
        if(hooks&&hooks->closing)hooks->closing(session,ui);
        persistSession(true);
        if(toolkit)Log(LogLevel::Info,"EditorUi.ToolkitPersisted");
        if(toolkit){
#if defined(FLARE_LOCAL_VALIDATION)
            return smoke.FinalResult(result);
#else
            return result;
#endif
        }
        frontend.FinishRecovery(result==0&&frontend.CloseApproved());
        if(result==0&&frontend.CloseApproved()&&!frontend.PublishRestartRequest())return 1;
#if defined(FLARE_LOCAL_VALIDATION)
        return smoke.FinalResult(result);
#else
        return result;
#endif
    }
    catch(const std::exception& exception)
    {Log(LogLevel::Error,std::string("EditorUi.StartFailed: ")+exception.what());return 1;}
}
}
