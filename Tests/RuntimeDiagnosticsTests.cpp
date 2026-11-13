#include "CoreLayer/Log.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Dred.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    const auto root=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(root);
    const auto file=root/"flush-and-threads.log";
    std::error_code error;std::filesystem::remove(file,error);
    if(!StartFileLog(file))return 3;
    Log(LogLevel::Debug,"must_be_filtered");
    std::vector<std::thread> threads;
    for(int worker=0;worker<4;++worker)threads.emplace_back([worker]{for(int i=0;i<30;++i)LogCheckpoint("Test.Concurrent","worker="+std::to_string(worker),i);});
    for(auto& thread:threads)thread.join();
    Log(LogLevel::Error,"Test.ErrorPersisted");
    std::ifstream input(file,std::ios::binary);const std::string contents{std::istreambuf_iterator<char>(input),{}};
    std::size_t count=0,position=0;while((position=contents.find("stage=Test.Concurrent",position))!=std::string::npos){++count;++position;}
    if(count!=120||contents.find("must_be_filtered")!=std::string::npos||contents.find("Test.ErrorPersisted")==std::string::npos)return 4;
    if(!SampleDiagnosticFrame(1)||SampleDiagnosticFrame(99)||!SampleDiagnosticFrame(600))return 5;
    // 仅配置 DRED，不创建设备/窗口，也不触发设备移除。
    long hr=0;const bool enabled=se::d3d12::EnableDred(&hr);
    std::cout<<"DRED settings: enabled="<<enabled<<" hr=0x"<<std::hex<<static_cast<unsigned long>(hr)<<'\n';
    if(!enabled||hr<0)return 6;
    if(!se::d3d12::DumpDred(nullptr,root/"dred-no-device.txt"))return 7;
    std::cout<<"Runtime diagnostics: passed (no GPU device/window created)\n";return 0;
}
