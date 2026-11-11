#include "Gallery/DemoPreset.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==3,"supply preset and test-output directories");const auto root=std::filesystem::absolute(argv[1]),output=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(output);std::vector<AssetDiagnostic> diagnostics;
        DemoPreset baseline;Check(LoadDemoPreset(root/"materials.json",baseline,diagnostics),"materials preset");
        for(const auto name:{"materials","shadows","postprocess"})
        {DemoPreset loaded;Check(LoadDemoPreset(root/(std::string(name)+".json"),loaded,diagnostics),"all presets load");Check(loaded.width==960&&loaded.height==720&&loaded.captureFrame==30&&loaded.fixedDelta==1.0/60,"common dimensions/frame/step");
            Check(loaded.camera.position.x==baseline.camera.position.x&&loaded.camera.position.y==baseline.camera.position.y&&loaded.camera.position.z==baseline.camera.position.z&&loaded.camera.yaw==baseline.camera.yaw&&loaded.camera.pitch==baseline.camera.pitch,"one shared camera");}
        std::vector<std::uint8_t> original;Check(asset_io::Read(root/"materials.json",original),"preset bytes");const auto valid=nlohmann::json::parse(original);const auto invalidPath=output/"invalid.json";
        for(int test=0;test<9;++test)
        {
            auto invalid=valid;switch(test){case 0:invalid["schema"]=2;break;case 1:invalid["size"][0]=-1;break;case 2:invalid["size"][0]=960.0;break;
            case 3:invalid["size"]={16384,16384};break;case 4:invalid["camera"]["pitch"]=2;break;case 5:invalid["fixed_delta"]=0;break;case 6:invalid["fixed_delta"]=0.2;break;case 7:invalid["capture_frame"]=0;break;default:invalid["scene"]="unknown";break;}
            const auto bytes=invalid.dump();Check(asset_io::WriteAtomic(invalidPath,{reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()}),"bad preset fixture");DemoPreset preserved;preserved.width=123;
            Check(!LoadDemoPreset(invalidPath,preserved,diagnostics)&&preserved.width==123,"invalid preset rejected without partial publication");
        }
        const std::vector<std::uint8_t> rgba{255,0,0,99,0,255,0,33};const auto path=output/L"截图.ppm";
        Check(WritePpmImage(path,2,1,rgba,diagnostics),"atomic Unicode PPM publication");std::vector<std::uint8_t> ppm;Check(asset_io::Read(path,ppm),"read PPM");
        const std::string header="P6\n2 1\n255\n";std::vector<std::uint8_t> expected(header.begin(),header.end());expected.insert(expected.end(),{255,0,0,0,255,0});Check(ppm==expected,"P6 tightly packs RGB and ignores alpha");
        Check(!WritePpmImage(path,2,2,rgba,diagnostics),"invalid image rejected");std::vector<std::uint8_t> after;Check(asset_io::Read(path,after)&&after==ppm,"failed image preserves previous file bytes");
        std::cout<<"DemoPresetTests passed (shared camera, schema/range and atomic preservation, Unicode PPM).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"DemoPresetTests: "<<error.what()<<'\n';return 1;}
}
