#include "ToolLayer/Editor/EditorProcessRunner.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs=std::filesystem;
namespace
{
fs::path root;
void Mark(const std::wstring& name){std::ofstream(root/name)<<GetCurrentProcessId();}
std::wstring Self(){std::wstring value(32768,L'\0');value.resize(GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size())));return value;}
bool Spawn(const std::wstring& mode,const fs::path& output)
{
    auto command=se::BuildProcessCommandLine(Self(),{mode,output.wstring()});STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(Self().c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return false;
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return true;
}
LRESULT CALLBACK WindowProcedure(HWND window,UINT message,WPARAM wp,LPARAM lp)
{if(message==WM_CLOSE){Mark(L"clean.txt");DestroyWindow(window);PostQuitMessage(0);return 0;}return DefWindowProcW(window,message,wp,lp);}
}
int wmain(int argc,wchar_t** argv)
{
    if(argc<3)return 2;const std::wstring mode=argv[1];root=argv[2];fs::create_directories(root);
    if(mode==L"--external")
    {
        if(argc!=5||std::wstring(argv[4])!=L"文字\\folder \"quoted\"")return 3;HANDLE stop=OpenEventW(SYNCHRONIZE,FALSE,argv[3]);if(!stop)return 4;
        Mark(L"external.pid");const auto result=WaitForSingleObject(stop,15000);CloseHandle(stop);return result==WAIT_OBJECT_0?0:5;
    }
    if(mode==L"--external-host")
    {
        if(argc!=4)return 3;std::string error;
        if(!se::StartEditorProcess(Self(),{L"--external",root.wstring(),argv[3],L"文字\\folder \"quoted\""},root,error)){std::ofstream(root/"error.txt")<<error;return 6;}
        Mark(L"host.pid");return 0;
    }
    if(mode==L"--window")
    {
        WNDCLASSW type{};type.lpfnWndProc=WindowProcedure;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"SelfEngineCpuShutdownProbe";
        RegisterClassW(&type);auto window=CreateWindowW(type.lpszClassName,L"CPU shutdown probe",WS_OVERLAPPEDWINDOW,0,0,100,100,nullptr,nullptr,type.hInstance,nullptr);
        if(!window)return 7;Mark(L"ready.pid");MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return 0;
    }
    if(mode==L"--tree"){Mark(L"root.pid");if(!Spawn(L"--child",root))return 8;}
    else if(mode==L"--child"){Mark(L"child.pid");if(!Spawn(L"--leaf",root))return 8;}
    else if(mode==L"--orphan-root"){if(!Spawn(L"--leaf",root))return 8;while(!fs::exists(root/L"leaf.pid"))Sleep(5);return 0;}
    else if(mode==L"--leaf")Mark(L"leaf.pid");
    else if(mode==L"--mark"){Mark(L"executed.pid");return 0;}
    else return 9;
    Sleep(15000);return 0; // 所有测试子进程都有自身保底期限。
}
