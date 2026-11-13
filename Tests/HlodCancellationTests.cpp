#include "FunctionLayer/Rendering/HlodStreamer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/TaskQueue.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==2,"supply output directory");const auto root=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(root);
        std::vector<std::uint8_t> payload(3*1024*1024);for(std::size_t i=0;i<payload.size();++i)payload[i]=static_cast<std::uint8_t>(i%251);
        Check(se::asset_io::WriteAtomic(root/"chunks.bin",payload,true),"write read fixture");
        std::stop_source stop;std::vector<std::uint8_t> actual;
        Check(se::asset_io::Read(root/"chunks.bin",actual,stop.get_token())&&actual==payload,"cancellable overlapped chunk reader preserves data");
        stop.request_stop();actual={42};
        Check(!se::asset_io::Read(root/"chunks.bin",actual,stop.get_token())&&actual==std::vector<std::uint8_t>{42},"cancelled read publishes no partial bytes");
        se::HlodScene scene;scene.cellSize=64;scene.instanceCount=1;se::HlodCell cell;cell.radius=1;se::HlodPart part;
        part.mesh.vertices.resize(3);part.mesh.indices={0,1,2};cell.parts.push_back(part);scene.cells.push_back(cell);
        std::string error;Check(se::WriteHlodScene(root/"scene.sehlod",scene,error),"write valid scene");
        se::HlodScene parsed;std::stop_source active;
        Check(se::ReadHlodScene(root/"scene.sehlod",parsed,error,active.get_token())&&parsed.cells.size()==1,"normal cancellable parser");
        parsed.instanceCount=99;
        Check(!se::ReadHlodScene(root/"scene.sehlod",parsed,error,stop.get_token())&&error=="Hlod.Cancelled"&&parsed.instanceCount==99,"cancelled parser preserves old output");
        se::HlodStreamer streamer;
        for(int iteration=0;iteration<8;++iteration)
        {
            Check(streamer.Start(root/"scene.sehlod",error,30000),"start long delayed worker");
            const auto began=std::chrono::steady_clock::now();streamer.Cancel();
            Check(std::chrono::steady_clock::now()-began<1500ms,"cancel interrupts 30 second delay and joins promptly");
            Check(!streamer.Running()&&streamer.Poll(parsed,error)==se::HlodStreamer::State::Idle,"cancelled worker cannot publish stale scene");
        }
        Check(streamer.Start(root/"scene.sehlod",error),"restart after cancellation");
        auto state=se::HlodStreamer::State::Pending;const auto deadline=std::chrono::steady_clock::now()+3s;
        while(state==se::HlodStreamer::State::Pending&&std::chrono::steady_clock::now()<deadline){state=streamer.Poll(parsed,error);std::this_thread::sleep_for(1ms);}
        Check(state==se::HlodStreamer::State::Ready&&parsed.instanceCount==1,"new worker publishes only its result");
        Check(streamer.Poll(parsed,error)==se::HlodStreamer::State::Idle,"single delivery");
        se::TaskQueue tasks(1,4);std::promise<void> started;bool published=false;
        tasks.Submit([&](const se::CancellationToken& token){started.set_value();while(!token.IsCancelled())std::this_thread::sleep_for(1ms);return [&]{published=true;};});
        Check(started.get_future().wait_for(2s)==std::future_status::ready,"cooperative task started");tasks.Shutdown();
        Check(!published&&tasks.Pending()==0,"shutdown joins cooperative work and drops completion");
        std::cout<<"HLOD cancellation passed: chunk IO, preserved output, interrupted delays, stale result rejection, restart, task join; no GPU.\n";return 0;
    }
    catch(const std::exception& failure){std::cerr<<failure.what()<<'\n';return 1;}
}
