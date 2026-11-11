#include "ResourceLayer/Assets/AsyncAssetLoader.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace se;
using namespace std::chrono_literals;
namespace
{
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int wmain(int argc,wchar_t** argv)
{
    Localization text; std::string error;
    if (!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error)) { std::cerr<<"AsyncAssetProbe localization unavailable\n";return 1; }
    try
    {
        fs::path samples="Resources/Samples/Models",parent;std::string language="en-US";
        for(int index=1;index<argc;++index)
        {
            const std::wstring argument=argv[index];Check(index+1<argc,"arguments");
            if(argument==L"--samples")samples=argv[++index];
            else if(argument==L"--output-parent")parent=argv[++index];
            else if(argument==L"--language"){const std::wstring value=argv[++index];Check(value==L"en-US"||value==L"zh-CN","language");language=value==L"zh-CN"?"zh-CN":"en-US";}
            else throw std::invalid_argument("arguments");
        }
        Check(!parent.empty()&&fs::is_directory(samples),"configuration");Check(text.Load(GetExecutableDirectory()/"Resources/Localization"/(language+".txt"),error),"localization");
        const auto root=fs::absolute(parent)/asset_io::NewId(),source=root/"source";
        Check(!fs::exists(root)&&fs::create_directories(source),"new output directory");
        for(const auto& entry:fs::directory_iterator(samples))if(entry.is_regular_file())fs::copy_file(entry.path(),source/entry.path().filename());
        const std::vector<std::uint8_t> broken{1,2,3,4};Check(asset_io::WriteAtomic(source/"broken.png",broken),"broken test image");
        AssetStore assets(source,root/"cache");TaskQueue tasks(2,16);AsyncAssetLoader loader(assets,tasks);
        std::map<AssetId,std::string> labels;std::vector<AssetTicket> models;std::vector<AssetDiagnostic> diagnostics;AssetId png,bad;
        for(const auto& name:{"Hierarchy.glb","Hierarchy.fbx","Hierarchy.gltf"})
        {AssetId id;Check(assets.RegisterSource(name,{},id,diagnostics),"model catalog");labels[id]=name;models.push_back(loader.Request(id,AsyncAssetKind::Model));}
        Check(assets.RegisterSource("checker.png",{},png,diagnostics)&&assets.RegisterSource("broken.png",{},bad,diagnostics),"image catalog");labels[png]="checker.png";labels[bad]="broken.png";
        auto cpu=loader.Request(png,AsyncAssetKind::Image),render=loader.Request(png,AsyncAssetKind::Image,AssetTarget::Renderable),cancelled=loader.Request(png,AsyncAssetKind::Image,AssetTarget::Renderable);cancelled.Cancel();
        auto failed=loader.Request(bad,AsyncAssetKind::Image);std::map<AssetId,std::pair<AssetLoadState,AssetLoadState>> previous;nlohmann::json history=nlohmann::json::array();
        std::cout<<text.GetText("Assets.ProbeGpuBoundary")<<'\n'<<text.GetText("Assets.ProbeHeading")<<'\n';
        const auto print=[&]{for(const auto& row:loader.StatusRows())
        {
            const auto states=std::make_pair(row.cpu,row.render);const auto old=previous.find(row.id);if(old!=previous.end()&&old->second==states)continue;previous[row.id]=states;
            std::cout<<labels[row.id]<<" | "<<text.GetText(AssetLoadStateKey(row.cpu))<<" | "<<text.GetText(AssetLoadStateKey(row.render))<<" | "<<row.clients<<" | "<<row.cpuBytes<<'\n';
            history.push_back({{"asset",labels[row.id]},{"cpu",AssetLoadStateKey(row.cpu)},{"render",AssetLoadStateKey(row.render)},{"clients",row.clients},{"cpu_bytes",row.cpuBytes}});
        }};
        print();bool finished=false;const auto deadline=std::chrono::steady_clock::now()+10s;
        while(std::chrono::steady_clock::now()<deadline)
        {
            loader.Pump();print();finished=loader.Query(cpu).state==AssetLoadState::Ready&&loader.Query(render).state==AssetLoadState::UploadQueued&&loader.Query(failed).state==AssetLoadState::Failed;
            for(const auto& model:models)finished=finished&&loader.Query(model).state==AssetLoadState::Ready;
            if(finished)break;if(tasks.Pending())tasks.WaitForReady(100ms);
        }
        const auto stats=loader.Stats();Check(finished&&loader.Query(cancelled).state==AssetLoadState::Cancelled&&stats.decodeStarts==5&&stats.sharedRequests==2,"CPU status completion");
        const nlohmann::json report{{"schema",1},{"success",true},{"mode","cpu-state-inspection"},{"real_gpu_upload",false},{"renderable_state",AssetLoadStateKey(loader.Query(render).state)},
            {"decode_starts",stats.decodeStarts},{"shared_requests",stats.sharedRequests},{"cpu_bytes",stats.cpuBytes},{"peak_cpu_bytes",stats.peakCpuBytes},{"history",std::move(history)}};
        const auto json=report.dump(2)+"\n";Check(asset_io::WriteAtomic(root/"run.json",{reinterpret_cast<const std::uint8_t*>(json.data()),json.size()}),"status report");
        std::cout<<text.GetText("Assets.ProbeComplete")<<": "<<asset_io::Utf8(root)<<'\n';return 0;
    }
    catch(const std::exception& exception){std::cerr<<text.GetText("Assets.ProbeFailed")<<": "<<exception.what()<<'\n';return 2;}
}
