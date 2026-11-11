#include "EditorLauncher.h"
#include "EditorWorker.h"
#include "ProjectWorkspaceSession.h"
#include "CoreLayer/Localization.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace se
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr int ControlNewProject=1001,ControlOpenProject=1002,ControlOpenSample=1004,ControlQuit=1005;
constexpr int ControlRecentList=1010,ControlRecentOpen=1011,ControlRecentRemove=1012,ControlRecentRelocate=1013;
constexpr int ControlTemplate=2001,ControlName=2002,ControlParent=2003,ControlBrowse=2004,ControlPreview=2005,ControlCreate=2006,ControlCancel=2007;
constexpr int WindowWidth=760,WindowHeight=520;
constexpr int DialogWidth=520,DialogHeight=300;

std::wstring Wide(std::string_view text)
{
    if(text.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(size<=0)return {};
    std::wstring result(static_cast<size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size);
    return result;
}
std::string Utf8(const std::wstring& text)
{
    if(text.empty())return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(size<=0)return {};
    std::string result(static_cast<size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr);
    return result;
}

struct LauncherStrings
{
    Localization localization;
    std::wstring title,status,environment,newProject,openProject,openSample,quit;
    std::wstring newSoon,recentEmpty,sampleMissing,invalidProject,workerFailed,safeMode,openTitle;
    std::wstring recentTitle,open,remove,relocate,selectHint,newTitle,templateLabel,templateBlank,templateBasic;
    std::wstring nameLabel,parentLabel,browse,finalPath,create,cancel,leaseBusy,openFailed,createFailed,preferenceFailed;
    std::wstring Get(std::string_view key,const wchar_t* fallback) const
    {
        const std::string localized=localization.GetText(key);
        if(localized.empty()||localized==key)return fallback;
        return Wide(localized);
    }
    std::wstring Error(std::string_view key) const
    {
        const std::string localized=localization.GetText(key);
        if(localized.empty()||localized==key)return L"["+Wide(key)+L"]";
        return Wide(localized);
    }
    static LauncherStrings Load(const std::filesystem::path& directory,const std::string& locale)
    {
        LauncherStrings strings;
        std::string error;
        strings.localization.Load(directory/("Resources/Localization/"+locale+".txt"),error);
        const auto value=[&](const char* key,const wchar_t* fallback){return strings.Get(key,fallback);};
        strings.title=value("Editor.LauncherTitle",L"SelfEngine Editor");
        strings.status=value("Editor.LauncherStatus",L"Editor entry ready; no project opened.");
        strings.environment=value("Editor.LauncherEnvironment",L"Runtime:");
        strings.newProject=value("Editor.LauncherNewProject",L"New Project");
        strings.openProject=value("Editor.LauncherOpenProject",L"Open Project...");
        strings.openSample=value("Editor.LauncherOpenSample",L"Open Sample");
        strings.quit=value("Editor.LauncherQuit",L"Quit");
        strings.newSoon=value("Editor.LauncherNewProjectSoon",L"Creating projects is planned for U02.");
        strings.recentEmpty=value("Editor.LauncherRecentEmpty",L"No recent project yet; open an existing project or the sample.");
        strings.sampleMissing=value("Editor.LauncherSampleMissing",L"The sample project is not shipped with this build.");
        strings.invalidProject=value("Editor.LauncherInvalidProject",L"Project manifest is missing or invalid:");
        strings.workerFailed=value("Editor.LauncherWorkerFailed",L"Editor worker exited with code:");
        strings.safeMode=value("Editor.LauncherSafeMode",L"Safe mode: session restore is skipped (restore arrives in U07).");
        strings.openTitle=value("Editor.LauncherOpenTitle",L"Select a project folder");
        strings.recentTitle=value("Editor.LauncherRecentTitle",L"Recent projects");
        strings.open=value("Editor.LauncherOpen",L"Open");
        strings.remove=value("Editor.LauncherRemove",L"Remove record");
        strings.relocate=value("Editor.LauncherRelocate",L"Relocate...");
        strings.selectHint=value("Editor.LauncherSelectHint",L"Double-click a recent project to open it.");
        strings.newTitle=value("Editor.LauncherNewTitle",L"New project");
        strings.templateLabel=value("Editor.LauncherTemplate",L"Template");
        strings.templateBlank=value("Editor.LauncherTemplateBlank",L"Blank scene");
        strings.templateBasic=value("Editor.LauncherTemplateBasic",L"Basic 3D sample");
        strings.nameLabel=value("Editor.LauncherName",L"Project name");
        strings.parentLabel=value("Editor.LauncherParent",L"Parent folder");
        strings.browse=value("Editor.LauncherBrowse",L"Browse...");
        strings.finalPath=value("Editor.LauncherFinalPath",L"Final folder:");
        strings.create=value("Editor.LauncherCreate",L"Create");
        strings.cancel=value("Editor.LauncherCancel",L"Cancel");
        strings.leaseBusy=value("Editor.LauncherLeaseBusy",L"Another editor instance is already editing this project.");
        strings.openFailed=value("Editor.LauncherOpenFailed",L"Cannot open project:");
        strings.createFailed=value("Editor.LauncherCreateFailed",L"Cannot create project:");
        strings.preferenceFailed=value("Editor.LauncherPreferenceFailed",L"Preferences could not be saved; this session can continue.");
        return strings;
    }
};

struct LauncherState
{
    LauncherStrings strings;
    std::filesystem::path executable,resourceDirectory,sample,userDirectory,templateRoot,smokeCreateParent;
    std::string locale;
    bool safeMode=false,uiErrors=true;
    ProjectWorkspaceSession workspace;
    HWND window=nullptr,title=nullptr,status=nullptr,environment=nullptr,note=nullptr;
    HWND newProject=nullptr,openProject=nullptr,openSample=nullptr,quit=nullptr;
    HWND recentList=nullptr,recentOpen=nullptr,recentRemove=nullptr,recentRelocate=nullptr;
    HFONT titleFont=nullptr,bodyFont=nullptr;
};

void ShowError(const LauncherState& state,const std::wstring& detail)
{
    std::wcerr<<L"[Error] launcher error\n";
    if(state.uiErrors)MessageBoxW(state.window,detail.c_str(),L"SelfEngineEditor",MB_ICONERROR|MB_OK);
}
void ShowError(const LauncherStrings& strings,bool uiErrors,const std::wstring& detail)
{
    (void)strings;
    std::wcerr<<L"[Error] launcher error\n";
    if(uiErrors)MessageBoxW(nullptr,detail.c_str(),L"SelfEngineEditor",MB_ICONERROR|MB_OK);
}
void SetNote(LauncherState& state,const std::wstring& text)
{
    if(state.note)SetWindowTextW(state.note,text.c_str());
}
bool PackageProject(const LauncherState& state,const std::filesystem::path& requested)
{
    std::error_code error;
    return !requested.empty()&&std::filesystem::is_regular_file(state.resourceDirectory/L"editor-package.json",error)
        &&asset_io::Within(state.resourceDirectory,requested);
}
bool RejectPackageProject(const LauncherState& state,const std::filesystem::path& requested)
{
    if(!PackageProject(state,requested))return false;
    const auto message=state.strings.Get("Editor.PackageProjectReadOnly",L"Projects inside the editor package are read-only. Use Open Sample to create a copy, or create/copy the project outside the package before opening it.");
    std::wcerr<<L"[Error] Editor.PackageProjectReadOnly: "<<message<<L"\n";
    ShowError(state,message);return true;
}
std::filesystem::path PickFolder(HWND owner)
{
    ComPtr<IFileOpenDialog> dialog;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.GetAddressOf()))))return {};
    DWORD options=0;
    if(SUCCEEDED(dialog->GetOptions(&options)))dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
    if(FAILED(dialog->Show(owner)))return {};
    ComPtr<IShellItem> item;
    if(FAILED(dialog->GetResult(item.GetAddressOf())))return {};
    PWSTR path=nullptr;
    if(FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))||!path)return {};
    std::filesystem::path result(path);CoTaskMemFree(path);return result;
}
void RefreshRecent(LauncherState& state)
{
    if(!state.recentList)return;
    SendMessageW(state.recentList,LB_RESETCONTENT,0,0);
    for(const auto& project:state.workspace.Recent())
    {
        std::wstring line=Wide(project.name.empty()?project.path:project.name)+L"  —  "+Wide(project.path);
        if(!project.available)line=L"[missing] "+line;
        SendMessageW(state.recentList,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(line.c_str()));
    }
    const auto empty=state.workspace.Recent().empty();
    (void)empty;
    EnableWindow(state.recentOpen,!empty?TRUE:FALSE);
    EnableWindow(state.recentRemove,!empty?TRUE:FALSE);
    EnableWindow(state.recentRelocate,!empty?TRUE:FALSE);
}
bool StartWorker(const LauncherState& state,const std::filesystem::path& projectRoot,const std::wstring& extraArguments,HANDLE& process,DWORD& pid)
{
    if(RejectPackageProject(state,projectRoot))return false;
    // 内部 worker 与后续托管候选继承同一安全模式；不扩大用户参数表。
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SAFE_MODE",state.safeMode?L"1":nullptr);
    std::wstring command=L"\""+state.executable.wstring()+L"\" --worker --project \""+projectRoot.wstring()+L"\" --language "+Wide(state.locale);
    if(!extraArguments.empty())command+=L" "+extraArguments;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION information{};
    if(!CreateProcessW(state.executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,state.executable.parent_path().c_str(),&startup,&information))
    {
        std::wcerr<<L"[Error] cannot start editor worker, code="<<GetLastError()<<L"\n";return false;
    }
    process=information.hProcess;pid=information.dwProcessId;CloseHandle(information.hThread);
    return true;
}
void WaitWorker(HANDLE process,DWORD pid,DWORD& exitCode)
{
    std::wcout<<L"[Info] worker-spawned pid="<<pid<<L"\n";
    WaitForSingleObject(process,INFINITE);
    GetExitCodeProcess(process,&exitCode);
    CloseHandle(process);
}
bool OpenProjectFlow(LauncherState& state,const std::filesystem::path& requested,const std::wstring& extraArguments=L"")
{
    // Preflight 会探测写权限，因此包内拒绝必须在预检之前。
    if(RejectPackageProject(state,requested))return false;
    const auto check=ProjectWorkspaceSession::Preflight(requested);
    if(!check.ok)
    {
        ShowError(state,state.strings.openFailed+L"\n"+state.strings.Error(check.errorKey)+L"\n"+Wide(check.detail));
        return false;
    }
    HANDLE process=nullptr;DWORD pid=0;
    if(!StartWorker(state,check.root,extraArguments,process,pid))
    {
        ShowError(state,state.strings.workerFailed);
        return false;
    }
    state.workspace.RecordOpened(check);
    std::string saveError;
    if(!state.workspace.Save(saveError))SetNote(state,state.strings.preferenceFailed);
    RefreshRecent(state);
    ShowWindow(state.window,SW_HIDE);
    DWORD exitCode=0;
    WaitWorker(process,pid,exitCode);
    ShowWindow(state.window,SW_SHOW);SetForegroundWindow(state.window);
    RefreshRecent(state);
    if(exitCode==6){ShowError(state,state.strings.leaseBusy);SetNote(state,state.strings.leaseBusy);return true;}
    if(exitCode!=0){ShowError(state,state.strings.workerFailed+L" "+std::to_wstring(exitCode));return true;}
    SetNote(state,state.strings.status);
    return true;
}
bool OpenSampleFlow(LauncherState& state)
{
    if(!PackageProject(state,state.sample))return OpenProjectFlow(state,state.sample);
    const auto parent=state.userDirectory/L"Samples";
    if(RejectPackageProject(state,parent))return false;
    std::error_code error;std::filesystem::create_directories(parent,error);
    if(error){ShowError(state,state.strings.createFailed+L"\n"+state.strings.Error("Project.ParentMissing"));return false;}
    const auto token=asset_io::NewId();
    if(token.empty()){ShowError(state,state.strings.createFailed);return false;}
    // 每次生成独立副本，不覆盖用户编辑过的旧示例；它们仍保留在最近项目列表。
    ProjectPreflight copied;std::string key,detail;
    if(!ProjectWorkspaceSession::CreateProject(ProjectTemplate::Basic3D,"Sample-"+token,parent,state.templateRoot,copied,key,detail))
    {ShowError(state,state.strings.createFailed+L"\n"+state.strings.Error(key)+L"\n"+Wide(detail));return false;}
    return OpenProjectFlow(state,copied.root);
}

struct NewProjectDialogState
{
    LauncherState* launcher=nullptr;
    HWND window=nullptr,combo=nullptr,name=nullptr,parent=nullptr,preview=nullptr;
    ProjectPreflight created;
    bool accepted=false;
};
void UpdateNewProjectPreview(NewProjectDialogState& dialog)
{
    if(!dialog.window||!dialog.preview)return;
    wchar_t name[256]{},parent[1024]{};
    GetWindowTextW(dialog.name,name,256);GetWindowTextW(dialog.parent,parent,1024);
    std::filesystem::path target(parent);
    if(name[0])target/=name;
    SetWindowTextW(dialog.preview,target.wstring().c_str());
}
void CreateNewProjectDialogControls(NewProjectDialogState& dialog,const LauncherState& launcher)
{
    const auto instance=GetModuleHandleW(nullptr);
    const auto make=[&](const wchar_t* type,const std::wstring& text,DWORD style,int x,int y,int width,int height,int id,HWND& output)
    {
        output=CreateWindowExW(0,type,text.c_str(),style|WS_CHILD|WS_VISIBLE,x,y,width,height,dialog.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        if(output)SendMessageW(output,WM_SETFONT,reinterpret_cast<WPARAM>(launcher.bodyFont),TRUE);
        return output!=nullptr;
    };
    HWND label=nullptr;make(L"STATIC",launcher.strings.templateLabel,SS_LEFT,16,16,120,22,-1,label);
    make(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,150,14,340,120,ControlTemplate,dialog.combo);
    SendMessageW(dialog.combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(launcher.strings.templateBlank.c_str()));
    SendMessageW(dialog.combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(launcher.strings.templateBasic.c_str()));
    SendMessageW(dialog.combo,CB_SETCURSEL,0,0);
    make(L"STATIC",launcher.strings.nameLabel,SS_LEFT,16,52,120,22,-1,label);
    make(L"EDIT",L"NewProject",WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,150,50,340,24,ControlName,dialog.name);
    make(L"STATIC",launcher.strings.parentLabel,SS_LEFT,16,88,120,22,-1,label);
    make(L"EDIT",L"",WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,150,86,240,24,ControlParent,dialog.parent);
    make(L"BUTTON",launcher.strings.browse,BS_PUSHBUTTON|WS_TABSTOP,396,84,94,28,ControlBrowse,label);
    make(L"STATIC",launcher.strings.finalPath,SS_LEFT,16,126,120,22,-1,label);
    make(L"STATIC",L"",SS_LEFT|SS_PATHELLIPSIS|WS_BORDER,16,150,480,24,-1,dialog.preview);
    make(L"BUTTON",launcher.strings.create,BS_DEFPUSHBUTTON|WS_TABSTOP,290,220,100,30,ControlCreate,label);
    make(L"BUTTON",launcher.strings.cancel,BS_PUSHBUTTON|WS_TABSTOP,400,220,100,30,ControlCancel,label);
    SetWindowTextW(dialog.name,L"NewProject");
    wchar_t buffer[1024]{};
    GetCurrentDirectoryW(1024,buffer);
    SetWindowTextW(dialog.parent,buffer);
    UpdateNewProjectPreview(dialog);
}
LRESULT CALLBACK NewProjectProcedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    auto* dialog=reinterpret_cast<NewProjectDialogState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    switch(message)
    {
    case WM_NCCREATE:
    {
        auto* create=reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window,message,wParam,lParam);
    }
    case WM_CREATE:return 0;
    case WM_COMMAND:
        if(!dialog||!dialog->launcher)break;
        switch(LOWORD(wParam))
        {
        case ControlBrowse:
        {
            const auto folder=PickFolder(window);
            if(!folder.empty()){SetWindowTextW(dialog->parent,folder.wstring().c_str());UpdateNewProjectPreview(*dialog);}
            return 0;
        }
        case ControlCreate:
        {
            wchar_t nameBuffer[256]{},parentBuffer[1024]{};
            GetWindowTextW(dialog->name,nameBuffer,256);GetWindowTextW(dialog->parent,parentBuffer,1024);
            if(RejectPackageProject(*dialog->launcher,std::filesystem::path(parentBuffer)/nameBuffer))return 0;
            const auto templateIndex=static_cast<int>(SendMessageW(dialog->combo,CB_GETCURSEL,0,0));
            const auto projectTemplate=templateIndex==1?ProjectTemplate::Basic3D:ProjectTemplate::BlankScene;
            std::string errorKey,detail;
            if(!ProjectWorkspaceSession::CreateProject(projectTemplate,Utf8(nameBuffer),std::filesystem::path(parentBuffer),
                dialog->launcher->templateRoot,dialog->created,errorKey,detail))
            {
                const auto text=dialog->launcher->strings.createFailed+L"\n"+dialog->launcher->strings.Error(errorKey)+L"\n"+Wide(detail);
                MessageBoxW(window,text.c_str(),L"SelfEngineEditor",MB_ICONERROR|MB_OK);
                return 0;
            }
            dialog->accepted=true;DestroyWindow(window);return 0;
        }
        case ControlCancel:DestroyWindow(window);return 0;
        case ControlName:case ControlParent:
            if(HIWORD(wParam)==EN_CHANGE){UpdateNewProjectPreview(*dialog);return 0;}
            break;
        case ControlTemplate:
            if(HIWORD(wParam)==CBN_SELCHANGE){UpdateNewProjectPreview(*dialog);return 0;}
            break;
        default:break;
        }
        break;
    case WM_KEYDOWN:
        if(wParam==VK_ESCAPE){DestroyWindow(window);return 0;}
        break;
    case WM_CLOSE:DestroyWindow(window);return 0;
    default:break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
bool RunNewProjectDialog(LauncherState& launcher,bool autoCreate)
{
    const auto instance=GetModuleHandleW(nullptr);
    const wchar_t* className=L"SelfEngineEditorNewProject";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style=CS_HREDRAW|CS_VREDRAW;
    windowClass.lpfnWndProc=NewProjectProcedure;
    windowClass.hInstance=instance;
    windowClass.hCursor=LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    windowClass.lpszClassName=className;
    if(!RegisterClassExW(&windowClass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
    NewProjectDialogState dialog;dialog.launcher=&launcher;
    RECT rectangle{0,0,DialogWidth,DialogHeight};
    AdjustWindowRectEx(&rectangle,WS_OVERLAPPEDWINDOW,FALSE,0);
    EnableWindow(launcher.window,FALSE);
    dialog.window=CreateWindowExW(WS_EX_DLGMODALFRAME,className,launcher.strings.newTitle.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,
        launcher.window,nullptr,instance,&dialog);
    if(!dialog.window){EnableWindow(launcher.window,TRUE);return false;}
    CreateNewProjectDialogControls(dialog,launcher);
    if(autoCreate)
    {
        SetWindowTextW(dialog.parent,launcher.smokeCreateParent.wstring().c_str());
        SetWindowTextW(dialog.name,(L"SmokeProject"+std::to_wstring(GetTickCount64())).c_str());
        SendMessageW(dialog.combo,CB_SETCURSEL,0,0);
        UpdateNewProjectPreview(dialog);
        PostMessageW(dialog.window,WM_COMMAND,MAKEWPARAM(ControlCreate,BN_CLICKED),0);
    }
    MSG message{};
    while(IsWindow(dialog.window))
    {
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        Sleep(10);
    }
    EnableWindow(launcher.window,TRUE);SetForegroundWindow(launcher.window);
    if(!dialog.accepted)return false;
    return OpenProjectFlow(launcher,dialog.created.root,autoCreate?L"--hidden --frames 20":L"");
}

LRESULT CALLBACK LauncherProcedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    auto* state=reinterpret_cast<LauncherState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    switch(message)
    {
    case WM_NCCREATE:
    {
        auto* create=reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window,message,wParam,lParam);
    }
    case WM_CREATE:return 0;
    case WM_COMMAND:
        if(!state)break;
        switch(LOWORD(wParam))
        {
        case ControlNewProject:
            if(RunNewProjectDialog(*state,false))SetNote(*state,state->strings.status);
            else SetNote(*state,state->strings.createFailed);
            return 0;
        case ControlOpenProject:
        {
            const auto folder=PickFolder(window);
            if(!folder.empty())OpenProjectFlow(*state,folder);
            return 0;
        }
        case ControlOpenSample:
            if(!std::filesystem::exists(state->sample/L"project.json")){SetNote(*state,state->strings.sampleMissing);ShowError(*state,state->strings.sampleMissing);return 0;}
            OpenSampleFlow(*state);
            return 0;
        case ControlRecentOpen:
        case ControlRecentList:
        {
            if(LOWORD(wParam)==ControlRecentList&&HIWORD(wParam)!=LBN_DBLCLK)break;
            const auto index=static_cast<int>(SendMessageW(state->recentList,LB_GETCURSEL,0,0));
            if(index<0||static_cast<size_t>(index)>=state->workspace.Recent().size())return 0;
            OpenProjectFlow(*state,state->workspace.Recent()[static_cast<size_t>(index)].path);
            return 0;
        }
        case ControlRecentRemove:
        {
            const auto index=static_cast<int>(SendMessageW(state->recentList,LB_GETCURSEL,0,0));
            if(index<0||static_cast<size_t>(index)>=state->workspace.Recent().size())return 0;
            state->workspace.RemoveRecent(state->workspace.Recent()[static_cast<size_t>(index)].path);
            std::string error;state->workspace.Save(error);
            RefreshRecent(*state);
            return 0;
        }
        case ControlRecentRelocate:
        {
            const auto index=static_cast<int>(SendMessageW(state->recentList,LB_GETCURSEL,0,0));
            if(index<0||static_cast<size_t>(index)>=state->workspace.Recent().size())return 0;
            const auto folder=PickFolder(window);
            if(folder.empty())return 0;
            if(RejectPackageProject(*state,folder))return 0;
            const auto check=ProjectWorkspaceSession::Preflight(folder);
            if(!check.ok){ShowError(*state,state->strings.openFailed+L"\n"+state->strings.Error(check.errorKey));return 0;}
            state->workspace.RemoveRecent(state->workspace.Recent()[static_cast<size_t>(index)].path);
            OpenProjectFlow(*state,folder);
            return 0;
        }
        case ControlQuit:PostMessageW(window,WM_CLOSE,0,0);return 0;
        default:break;
        }
        break;
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    default:break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

bool CreateControls(LauncherState& state)
{
    const auto instance=GetModuleHandleW(nullptr);
    const auto make=[&](const wchar_t* type,const std::wstring& text,DWORD style,int x,int y,int width,int height,int id,HWND& output)
    {
        output=CreateWindowExW(0,type,text.c_str(),style|WS_CHILD|WS_VISIBLE,x,y,width,height,state.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        if(output)SendMessageW(output,WM_SETFONT,reinterpret_cast<WPARAM>(state.bodyFont),TRUE);
        return output!=nullptr;
    };
    if(!make(L"STATIC",state.strings.title,SS_LEFT,24,18,WindowWidth-48,36,-1,state.title))return false;
    SendMessageW(state.title,WM_SETFONT,reinterpret_cast<WPARAM>(state.titleFont),TRUE);
    if(!make(L"STATIC",state.strings.status,SS_LEFT,24,60,WindowWidth-48,22,-1,state.status))return false;
    const std::wstring environment=state.strings.environment+L" "+Wide(state.locale)+L" | "+state.resourceDirectory.wstring();
    if(!make(L"STATIC",environment,SS_LEFT|SS_PATHELLIPSIS,24,82,WindowWidth-48,22,-1,state.environment))return false;
    const auto button=[&](const std::wstring& text,int x,int y,int width,int height,int id,bool enabled,HWND& output)
    {
        if(!make(L"BUTTON",text,BS_PUSHBUTTON|WS_TABSTOP,x,y,width,height,id,output))return false;
        EnableWindow(output,enabled?TRUE:FALSE);
        return true;
    };
    if(!button(state.strings.newProject,24,116,220,32,ControlNewProject,true,state.newProject))return false;
    if(!button(state.strings.openProject,264,116,220,32,ControlOpenProject,true,state.openProject))return false;
    if(!button(state.strings.openSample,504,116,220,32,ControlOpenSample,true,state.openSample))return false;
    HWND recentLabel=nullptr;if(!make(L"STATIC",state.strings.recentTitle,SS_LEFT,24,164,WindowWidth-48,22,-1,recentLabel))return false;
    if(!make(L"LISTBOX",L"",WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_TABSTOP,24,188,700,150,ControlRecentList,state.recentList))return false;
    if(!button(state.strings.open,24,348,140,30,ControlRecentOpen,true,state.recentOpen))return false;
    if(!button(state.strings.remove,176,348,140,30,ControlRecentRemove,true,state.recentRemove))return false;
    if(!button(state.strings.relocate,328,348,160,30,ControlRecentRelocate,true,state.recentRelocate))return false;
    if(!button(state.strings.quit,584,348,140,30,ControlQuit,true,state.quit))return false;
    if(!make(L"STATIC",state.strings.selectHint,SS_LEFT,24,390,700,44,-1,state.note))return false;
    RefreshRecent(state);
    return true;
}

bool CaptureWindow(HWND window,const std::filesystem::path& path)
{
    RECT client{};if(!GetClientRect(window,&client))return false;
    const int width=client.right-client.left,height=client.bottom-client.top;
    if(width<=0||height<=0)return false;
    const HDC windowDc=GetDC(window);if(!windowDc)return false;
    const HDC memoryDc=CreateCompatibleDC(windowDc);
    const HBITMAP bitmap=CreateCompatibleBitmap(windowDc,width,height);
    const HGDIOBJ previous=SelectObject(memoryDc,bitmap);
    const BOOL captured=PrintWindow(window,memoryDc,PW_CLIENTONLY);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::vector<std::uint8_t> pixels(static_cast<size_t>(width)*static_cast<size_t>(height)*4);
    const int rows=GetDIBits(windowDc,bitmap,0,static_cast<UINT>(height),pixels.data(),&info,DIB_RGB_COLORS);
    SelectObject(memoryDc,previous);DeleteObject(bitmap);DeleteDC(memoryDc);ReleaseDC(window,windowDc);
    if(!captured||rows!=height)return false;
    BITMAPFILEHEADER file{};BITMAPINFOHEADER header=info.bmiHeader;
    file.bfType=0x4D42;file.bfOffBits=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));
    output.write(reinterpret_cast<const char*>(&header),sizeof(header));
    output.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    return bool(output);
}
int RunLauncherWindow(LauncherState& state,const EditorLaunchOptions& options,std::uint64_t smokeMilliseconds,const std::filesystem::path& smokeProject){
    const auto instance=GetModuleHandleW(nullptr);
    const wchar_t* className=L"SelfEngineEditorLauncher";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style=CS_HREDRAW|CS_VREDRAW;
    windowClass.lpfnWndProc=LauncherProcedure;
    windowClass.hInstance=instance;
    windowClass.hCursor=LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    windowClass.lpszClassName=className;
    if(!RegisterClassExW(&windowClass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 5;
    RECT rectangle{0,0,WindowWidth,WindowHeight};
    AdjustWindowRectEx(&rectangle,WS_OVERLAPPEDWINDOW,FALSE,0);
    state.window=CreateWindowExW(0,className,state.strings.title.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,
        nullptr,nullptr,instance,&state);
    if(!state.window)return 5;
    state.titleFont=CreateFontW(-26,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    state.bodyFont=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
    if(!CreateControls(state)){DestroyWindow(state.window);return 5;}
    if(state.safeMode)SetNote(state,state.strings.safeMode);
    UpdateWindow(state.window);
    FILETIME processCreated{},exited{},kernel{},user{},now{};
    if(GetProcessTimes(GetCurrentProcess(),&processCreated,&exited,&kernel,&user))
    {
        GetSystemTimeAsFileTime(&now);
        const auto ticks=[](FILETIME value){return (static_cast<std::uint64_t>(value.dwHighDateTime)<<32)|value.dwLowDateTime;};
        if(ticks(now)>=ticks(processCreated))std::wcout<<L"[Info] launcher-interactive-ms="<<(ticks(now)-ticks(processCreated))/10000<<L"\n";
    }
    if(!options.captureLauncher.empty())
    {
        MSG paint{};
        for(int index=0;index<20;++index)
        {
            while(PeekMessageW(&paint,nullptr,0,0,PM_REMOVE)){TranslateMessage(&paint);DispatchMessageW(&paint);}
            Sleep(10);
        }
        if(CaptureWindow(state.window,options.captureLauncher))std::wcout<<L"[Info] launcher-capture="<<options.captureLauncher.wstring()<<L"\n";
        else std::wcerr<<L"[Error] launcher capture failed\n";
    }
    std::wcout<<L"[Info] launcher-ready buttons=8 recent="<<state.workspace.Recent().size()<<L"\n";
    int exitCode=0;
    std::uint64_t loopMilliseconds=smokeMilliseconds;
    if(!options.smokeCreateParent.empty())
    {
        ShowWindow(state.window,SW_HIDE);
        const bool created=RunNewProjectDialog(state,true);
        std::wcout<<L"[Info] smoke-create="<<(created?1u:0u)<<L"\n";
        ShowWindow(state.window,SW_SHOW);
        if(!created)exitCode=4;
        loopMilliseconds=100;
    }
    if(!smokeProject.empty())
    {
        const auto check=RejectPackageProject(state,smokeProject)?ProjectPreflight{}:ProjectWorkspaceSession::Preflight(smokeProject);
        if(!check.ok){std::wcerr<<L"[Error] smoke project invalid\n";exitCode=3;}
        else
        {
            ShowWindow(state.window,SW_HIDE);
            HANDLE process=nullptr;DWORD pid=0;DWORD workerExit=0;
            const bool started=StartWorker(state,check.root,L"--hidden --frames 20",process,pid);
            if(started)WaitWorker(process,pid,workerExit);
            std::wcout<<L"[Info] smoke-worker-exit="<<(started?workerExit:1u)<<L"\n";
            ShowWindow(state.window,SW_SHOW);
            if(!started||workerExit!=0)exitCode=4;
        }
    }
    const DWORD start=GetTickCount();
    MSG message{};
    while(true)
    {
        if(loopMilliseconds)
        {
            if(GetTickCount()-start>=loopMilliseconds)break;
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))
            {
                if(message.message==WM_QUIT){exitCode=0;goto finished;}
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            Sleep(10);
        }
        else
        {
            if(GetMessageW(&message,nullptr,0,0)<=0)break;
            TranslateMessage(&message);DispatchMessageW(&message);
        }
    }
finished:
    if(IsWindow(state.window))DestroyWindow(state.window);
    if(state.titleFont)DeleteObject(state.titleFont);
    if(state.bodyFont)DeleteObject(state.bodyFont);
    std::wcout<<L"[Info] launcher-exit="<<exitCode<<L"\n";
    return exitCode;
}

bool ParseUnsigned(const std::wstring& value,std::uint64_t maximum,std::uint64_t& output)
{
    if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos)return false;
    try
    {
        std::size_t consumed=0;output=std::stoull(value,&consumed);
        return consumed==value.size()&&output<=maximum;
    }
    catch(...){return false;}
}
}

bool ParseEditorLaunchOptions(int argc,wchar_t** argv,EditorLaunchOptions& options,std::string& error)
{
    for(int index=1;index<argc;++index)
    {
        const std::wstring argument=argv[index];
        const auto next=[&](std::wstring& output)->bool{if(index+1>=argc)return false;output=argv[++index];return true;};
        std::wstring value;
        if(argument==L"--project"){if(!next(value)){error="--project requires a value";return false;}options.project=value;}
        else if(argument==L"--language")
        {
            if(!next(value)){error="--language requires a value";return false;}
            if(value==L"zh-CN")options.locale="zh-CN";else if(value==L"en-US")options.locale="en-US";else{error="--language accepts zh-CN or en-US";return false;}
        }
        else if(argument==L"--editor-ui")
        {
            if(!next(value)){error="--editor-ui requires imgui or toolkit";return false;}
            if(value==L"imgui")options.editorUi="imgui";else if(value==L"toolkit")options.editorUi="toolkit";else{error="--editor-ui accepts imgui or toolkit";return false;}
        }
        else if(argument.rfind(L"--editor-ui=",0)==0)
        {
            const std::wstring inlineValue=argument.substr(12);
            if(inlineValue==L"imgui")options.editorUi="imgui";else if(inlineValue==L"toolkit")options.editorUi="toolkit";else{error="--editor-ui accepts imgui or toolkit";return false;}
        }
        else if(argument==L"--safe-mode")options.safeMode=true;
        else if(argument==L"--worker")options.worker=true;
        else if(argument==L"--hidden")options.hidden=true;
        else if(argument==L"--demo")options.demo=true;
        else if(argument==L"--ui-errors")options.uiErrors=true;
        else if(argument==L"--no-ui-errors")options.uiErrors=false;
        else if(argument==L"--scene"){if(!next(value)){error="--scene requires a value";return false;}options.scene=value;}
        else if(argument==L"--capture"){if(!next(value)){error="--capture requires a value";return false;}options.capture=value;}
        else if(argument==L"--capture-launcher"){if(!next(value)){error="--capture-launcher requires a value";return false;}options.captureLauncher=value;}
        else if(argument==L"--frames")
        {
            if(!next(value)||!ParseUnsigned(value,1'000'000,options.frames)||options.frames==0){error="--frames requires a positive count";return false;}
        }
        else if(argument==L"--smoke-launcher")
        {
            if(!next(value)||!ParseUnsigned(value,60'000,options.smokeLauncherMs)){error="--smoke-launcher requires milliseconds";return false;}
        }
        else if(argument==L"--smoke-open"){if(!next(value)){error="--smoke-open requires a project path";return false;}options.smokeOpenProject=value;}
        else if(argument==L"--smoke-create"){if(!next(value)){error="--smoke-create requires a parent path";return false;}options.smokeCreateParent=value;}
        else if(argument==L"--smoke-export"){if(!next(value)){error="--smoke-export requires a name";return false;}options.smokeExportName=Utf8(value);}
        else if(argument==L"--smoke-report"){if(!next(value)){error="--smoke-report requires a file path";return false;}options.smokeExportReport=value;}
        else if(argument==L"--smoke-playback")options.smokePlayback=true;
        else if(argument==L"--smoke-workflow"){if(!next(value)){error="--smoke-workflow requires a file path";return false;}options.smokeWorkflow=value;}
        else if(argument==L"--smoke-cancel-ms")
        {
            if(!next(value)||!ParseUnsigned(value,600'000,options.smokeCancelMilliseconds)){error="--smoke-cancel-ms requires milliseconds";return false;}
        }
        else{error="unknown argument";return false;}
    }
    if(options.worker&&options.project.empty()){error="--worker requires --project";return false;}
    if(options.hidden&&options.frames==0&&options.smokeExportReport.empty()&&options.smokeWorkflow.empty()){error="--hidden requires --frames";return false;}
    if(!options.smokeExportName.empty()&&options.smokeExportReport.empty()){error="--smoke-export requires --smoke-report";return false;}
    if(!options.smokeExportReport.empty()&&options.smokeExportName.empty()&&!options.smokePlayback){error="--smoke-report requires --smoke-export or --smoke-playback";return false;}
    if(!options.smokeExportReport.empty()&&!options.worker){error="--smoke-export requires --worker";return false;}
    if(options.smokeCancelMilliseconds>0&&options.smokeExportReport.empty()){error="--smoke-cancel-ms requires --smoke-export";return false;}
    if(options.smokePlayback&&options.smokeExportReport.empty()){error="--smoke-playback requires --smoke-report";return false;}
    if(!options.smokeWorkflow.empty()&&(!options.worker||options.frames||options.demo||!options.smokeExportReport.empty()||options.smokePlayback))
    {error="--smoke-workflow requires --worker and cannot be combined with other smoke/frame/demo modes";return false;}
    if(options.demo&&!options.scene.empty()){error="--demo and --scene cannot be combined";return false;}
    return true;
}

int RunEditorLauncher(const EditorLaunchOptions& options)
{
    const auto executable=GetExecutableDirectory().wstring();
    if(executable.empty()){ShowError(LauncherStrings::Load({},options.locale),options.uiErrors,L"cannot resolve executable directory");return 5;}
    LauncherState state;
    state.executable=std::filesystem::path(executable)/L"SelfEngineEditor.exe";
    state.resourceDirectory=std::filesystem::path(executable);
    state.locale=options.locale;
    state.safeMode=options.safeMode;
    state.uiErrors=options.uiErrors;
    state.strings=LauncherStrings::Load(state.resourceDirectory,options.locale);
    state.sample=state.resourceDirectory/L"Resources/Templates/EmptyGame";
    state.templateRoot=state.resourceDirectory/L"Resources/Templates";
    state.smokeCreateParent=options.smokeCreateParent;
    const auto engineRoot=ProjectWorkspaceSession::EngineRootFromExecutable();
    state.userDirectory=ProjectWorkspaceSession::UserDataDirectory(engineRoot);
    std::string loadError;
    state.workspace.Load(state.userDirectory,loadError);
    if(!options.project.empty())
    {
        if(RejectPackageProject(state,options.project))return 3;
        const auto check=ProjectWorkspaceSession::Preflight(options.project);
        if(!check.ok)
        {
            ShowError(state.strings,options.uiErrors,state.strings.openFailed+L"\n"+state.strings.Error(check.errorKey)+L"\n"+Wide(check.detail));
            return 3;
        }
        HANDLE process=nullptr;DWORD pid=0;DWORD exitCode=0;
        std::wstring extra;
        if(options.hidden)extra+=L"--hidden ";
        if(options.frames)extra+=L"--frames "+std::to_wstring(options.frames)+L" ";
        if(!options.scene.empty())extra+=L"--scene \""+options.scene.wstring()+L"\" ";
        if(!options.capture.empty())extra+=L"--capture \""+options.capture.wstring()+L"\" ";
        if(options.demo)extra+=L"--demo ";
        if(!options.smokeExportName.empty())extra+=L"--smoke-export \""+Wide(options.smokeExportName)+L"\" --smoke-report \""+options.smokeExportReport.wstring()+L"\" ";
        if(options.smokeCancelMilliseconds)extra+=L"--smoke-cancel-ms "+std::to_wstring(options.smokeCancelMilliseconds)+L" ";
        if(!options.editorUi.empty())extra+=L"--editor-ui="+Wide(options.editorUi)+L" ";
        if(!StartWorker(state,check.root,extra,process,pid)){ShowError(state.strings,options.uiErrors,state.strings.workerFailed);return 4;}
        state.workspace.RecordOpened(check);std::string saveError;state.workspace.Save(saveError);
        WaitWorker(process,pid,exitCode);
        return static_cast<int>(exitCode);
    }
    struct Apartment
    {
        HRESULT value;
        ~Apartment(){if(value==S_OK||value==S_FALSE)CoUninitialize();}
    } apartment{CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)};
    if(FAILED(apartment.value)&&apartment.value!=RPC_E_CHANGED_MODE)return 5;
    return RunLauncherWindow(state,options,options.smokeLauncherMs,options.smokeOpenProject);
}
}
