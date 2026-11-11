#include "Benchmark/BenchmarkScene.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Math/Bounds.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
using namespace se;
namespace {void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==3,"supply benchmark config and output directories");const auto input=std::filesystem::absolute(argv[1]),output=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(output);
        std::vector<AssetDiagnostic> diagnostics;BenchmarkConfig repeated,mixed;Check(LoadBenchmarkConfig(input/"repeated.json",repeated,diagnostics)&&LoadBenchmarkConfig(input/"mixed.json",mixed,diagnostics),"both benchmark configs");
        Check(repeated.objects==1000&&repeated.seed==mixed.seed&&repeated.warmupSeconds==5&&repeated.sampleSeconds==60,"formal default protocol is explicit");
        BenchmarkLayout a,b,c;Check(BuildBenchmarkLayout(repeated,a)&&BuildBenchmarkLayout(repeated,b)&&a.objects.size()==1000&&a.side==10&&a.digest==b.digest,"same seed has identical count/layout digest");
        Check(BuildBenchmarkLayout(mixed,c)&&c.digest!=a.digest,"mixed workload differs");std::unordered_set<std::uint32_t> materials;size_t spheres=0;
        for(size_t index=0;index<c.objects.size();++index)
        {const auto& object=c.objects[index];spheres+=object.sphere;materials.insert(object.material);Check(object.transform.position.x==a.objects[index].transform.position.x&&object.transform.position.y==a.objects[index].transform.position.y&&object.transform.position.z==a.objects[index].transform.position.z,"scene variants share seeded positions");
            Check(Inverse(object.transform.Matrix()).has_value(),"all generated mirrored/nonuniform transforms remain invertible");}
        Check(spheres>300&&spheres<700&&materials.size()==24,"mixed includes both meshes and all material slots");
        Check(a.digest=="e48923e68997d20754962b25295a2b46251b3e96403faf31c18dd6dcdd71b0ad"&&c.digest=="bce50db31e211f56b1efaf8bbbf9d71e77cb52f0b4d6b8237d0a89ea6312a37e","T30 baseline identity is preserved");
        for(const auto* name:{"all_visible.json","mostly_outside.json"})
        {
            BenchmarkConfig variant;BenchmarkLayout layout;Check(LoadBenchmarkConfig(input/name,variant,diagnostics)&&BuildBenchmarkLayout(variant,layout),"visibility fixture config/layout");
            std::vector<Aabb> boxes;for(const auto& object:layout.objects){const auto box=TransformBounds({{-0.5f,-0.5f,-0.5f},{0.5f,0.5f,0.5f}},object.transform.Matrix());Check(box.has_value(),"visibility fixture world box");boxes.push_back(*box);}
            for(int angle=0;angle<120;++angle)
            {
                const auto pose=BenchmarkCamera(variant,layout.side,double(angle)*0.5);const auto clip=*PerspectiveRH(Pi/3,float(variant.width)/variant.height,0.1f,1000) * *InverseRigid(FreeCameraTransform(pose).Matrix());
                const auto frustum=BuildFrustum(clip);Check(frustum.has_value(),"visibility benchmark frustum");size_t visible=0;for(const auto& box:boxes)visible+=Intersects(*frustum,box);
                Check(visible==(variant.visibility==BenchmarkVisibility::AllVisible?1000:100),"all 120 camera positions preserve declared visibility workload");
            }
            const auto identity=layout.digest;variant.frustumCulling=true;Check(BuildBenchmarkLayout(variant,layout)&&layout.digest==identity,"culling toggle does not change geometry identity");
        }
        auto changed=repeated;++changed.seed;Check(BuildBenchmarkLayout(changed,b)&&b.digest!=a.digest,"seed changes object identity");changed=repeated;changed.objects=0;const auto previous=b.digest;
        Check(!BuildBenchmarkLayout(changed,b)&&b.digest==previous,"invalid generation does not publish partial output");changed=repeated;changed.objects=50001;Check(!ValidateBenchmarkConfig(changed),"bounded draw budget");
        const auto front=BenchmarkCamera(repeated,10,0),right=BenchmarkCamera(repeated,10,15),loop=BenchmarkCamera(repeated,10,60);
        Check(std::abs(front.position.x)<1e-6f&&std::abs(front.position.z-26)<1e-5f&&std::abs(front.position.y-6)<1e-5f,"route starts at defined point");
        Check(std::abs(right.position.x-26)<1e-5f&&std::abs(right.position.z)<1e-5f&&Length(loop.position-front.position)<1e-5f,"quarter circle and closed route");
        const auto direction=Normalize(TransformVector(FreeCameraTransform(right).Matrix(),{0,0,-1}));Check(Dot(direction,Normalize(-right.position))>0.99999f,"route camera targets world center");
        std::vector<std::uint8_t> bytes;Check(asset_io::Read(input/"mixed.json",bytes),"config bytes");const auto valid=nlohmann::json::parse(bytes);
        for(int test=0;test<7;++test){auto invalid=valid;switch(test){case 0:invalid["schema"]=2;break;case 1:invalid["seed"]=0;break;case 2:invalid["object_count"]=1000.0;break;case 3:invalid["size"]={8192,8192};break;case 4:invalid["sample_seconds"]=-1;break;case 5:invalid["camera_path"]["period_seconds"]=0;break;default:invalid["scene"]="other";break;}
            const auto text=invalid.dump();const auto path=output/"invalid.json";Check(asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()}),"invalid config fixture");BenchmarkConfig preserved;preserved.objects=123;
            Check(!LoadBenchmarkConfig(path,preserved,diagnostics)&&preserved.objects==123,"bad config is rejected atomically");}
        std::cout<<"BenchmarkTests passed; repeated layout SHA-256="<<a.digest<<"; mixed="<<c.digest<<"; spheres="<<spheres<<".\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"BenchmarkTests: "<<error.what()<<'\n';return 1;}
}
