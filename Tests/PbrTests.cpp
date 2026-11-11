#include "FunctionLayer/Rendering/StaticModelRender.h"
#include "FunctionLayer/Rendering/LodSelect.h"
#include "ResourceLayer/Assets/HlodBake.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Windows/Window.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/TextureCompression.h"
#include "CoreLayer/Localization.h"
#include "Materials/MaterialsDemo.h"
#include <atomic>
#include <iterator>
#include <thread>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
namespace rhi=se::rhi;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result result,const char* message){if(!result)throw std::runtime_error(std::string(message)+": "+result.Key());}
std::string Read(const fs::path& path)
{std::ifstream input(path,std::ios::binary);Check(bool(input),"read PBR fixture");return {std::istreambuf_iterator<char>(input),{}};}
void Replace(std::string& text,const std::string& before,const std::string& after)
{const auto position=text.find(before);Check(position!=std::string::npos,"replace PBR fixture text");text.replace(position,before.size(),after);}
void Save(const rhi::Image& image,const fs::path& path)
{
    std::ofstream stream(path,std::ios::binary);stream<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t index=0;index<image.rgba8.size();index+=4)stream.write(reinterpret_cast<const char*>(image.rgba8.data()+index),3);
    Check(bool(stream),"save PBR image");
}
void Cpu(const fs::path& output)
{
    const auto sphere=MakeUnitSphere(32,16);
    Check(sphere.vertices.size()==33*17&&sphere.indices.size()==6*32*15,"sphere counts exclude degenerate pole faces");
    for(const auto& vertex:sphere.vertices)
    {
        const Vec3 p{vertex.position[0],vertex.position[1],vertex.position[2]},n{vertex.normal[0],vertex.normal[1],vertex.normal[2]},t{vertex.tangent[0],vertex.tangent[1],vertex.tangent[2]};
        Check(std::abs(Length(p)-0.5f)<1e-6f&&std::abs(Length(n)-1)<1e-6f&&std::abs(Length(t)-1)<1e-6f&&std::abs(Dot(n,t))<1e-6f,"unit sphere normals and orthogonal tangents");
        Check(vertex.uv[0]>=0&&vertex.uv[0]<=1&&vertex.uv[1]>=0&&vertex.uv[1]<=1&&vertex.tangent[3]==1,"sphere UV and +V tangent handedness");
    }
    for(size_t index=0;index<sphere.indices.size();index+=3)
    {
        Vec3 p[3];for(size_t corner=0;corner<3;++corner){const auto& a=sphere.vertices[sphere.indices[index+corner]].position;p[corner]={a[0],a[1],a[2]};}
        Check(Dot(Cross(p[1]-p[0],p[2]-p[0]),p[0]+p[1]+p[2])>1e-8f,"sphere outward winding and nonzero triangle area");
    }
    for(size_t row=0;row<17;++row)Check(sphere.vertices[row*33].position==sphere.vertices[row*33+32].position,"sphere seam positions match exactly");
    bool rejected=false;try{MakeUnitSphere(2,2);}catch(const std::invalid_argument& e){rejected=std::string(e.what())=="Assets.SphereSubdivisionsInvalid";}Check(rejected,"sphere invalid range explicit");
    std::vector<AssetDiagnostic> diagnostics;MaterialDocument source;source.normalScale=2.5f;std::string bytes;
    Check(SerializeMaterialDocument(source,bytes,diagnostics),"normal scale serialization");
    auto json=nlohmann::json::parse(bytes);MaterialDocument loaded;
    Check(DeserializeMaterialDocument(bytes,loaded,diagnostics)&&loaded.normalScale==2.5f,"normal scale round trip");
    json.erase("normalScale");Check(DeserializeMaterialDocument(json.dump(),loaded,diagnostics)&&loaded.normalScale==1,"old material schema defaults to one");
    const auto resources=fs::absolute("Resources/Samples/Models");const auto root=output/"import";fs::create_directories(root);
    for(const auto* name:{"Hierarchy.gltf","Hierarchy.bin","checker.png"})fs::copy_file(resources/name,root/name,fs::copy_options::overwrite_existing);
    std::ifstream input(root/"Hierarchy.gltf");auto modelJson=nlohmann::json::parse(input);input.close();
    modelJson["materials"][0]["normalTexture"]={{"index",0},{"scale",2.5}};
    modelJson["materials"][0]["pbrMetallicRoughness"]["metallicRoughnessTexture"]={{"index",0}};
    std::ofstream(root/"Hierarchy.gltf")<<modelJson.dump(2);
    AssetStore assets(root,output/"import-cache");AssetId id;
    Check(assets.RegisterSource("Hierarchy.gltf",{},id,diagnostics),"register normal and MR source");
    auto model=assets.LoadModel(id,diagnostics);Check(model&&model->materials[0].normalScale==2.5f&&model->materials[0].normalTexture==0&&model->materials[0].metallicRoughnessTexture==0,"glTF normal scale and shared texture slots imported");
    AssetStore fresh(root,output/"import-cache");Check(fresh.Scan(diagnostics),"scan saved asset metadata");auto cached=fresh.LoadModel(id,diagnostics);
    Check(cached&&cached->materials[0].normalScale==2.5f,"derived model cache preserves scale");
}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;StaticModelRenderCache cache{true};rhi::Pipeline pipeline;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false),"PBR hidden window");RECT rect{0,0,320,240};
        AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"PBR window size");
        int code=0;Check(PumpWindowMessages(code),"PBR window messages");rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"PBR GPU device");
        Ok(device->ConfigureDescriptors({16,128,3,6}),"two three-slot material tables");
        Ok(device->CreatePipeline({rhi::PipelineKind::PbrMesh,GetExecutableDirectory()/"Resources/Shaders/Pbr.hlsl",2},pipeline),"PBR reflected layout 2 artifact");
        Check(device->Stats().shaderArtifactLoads==1&&device->Stats().shaderProgramCompilations==0,"PBR runtime uses built artifact");
    }
    ~Fixture(){if(device)device->Shutdown();cache.Clear();pipeline.reset();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
std::shared_ptr<const StaticRenderModel> Quad(Fixture& f)
{
    auto model=std::make_shared<StaticModel>();MeshData mesh;mesh.name="ReferenceQuad";mesh.material=0;
    mesh.vertices={{{-1,-1,0},{0,0,1},{0,1},{1,0,0,1}},{{1,-1,0},{0,0,1},{1,1},{1,0,0,1}},
        {{1,1,0},{0,0,1},{1,0},{1,0,0,1}},{{-1,1,0},{0,0,1},{0,0},{1,0,0,1}}};
    mesh.indices={0,1,2,0,2,3};model->meshes.push_back(mesh);model->materials.emplace_back();
    ModelNode node;node.name="Quad";node.meshes={0};model->nodes.push_back(node);
    std::shared_ptr<const StaticRenderModel> gpu;std::vector<AssetDiagnostic> diagnostics;
    Check(f.cache.Upload(model,{},fs::current_path(),*f.device,gpu,diagnostics),"PBR model and default three textures");return gpu;
}
rhi::Texture Texture(rhi::Device& device,std::array<std::uint8_t,4> value)
{rhi::Texture result;Ok(device.CreateTexture({1,1},value,result),"PBR test texture");return result;}
Mat4 VP(){return *OrthographicRH(4,3,0.1f,100)*Translation({0,0,-3});}
std::unique_ptr<rhi::CommandContext> Record(rhi::Device& device,std::span<const rhi::DrawIndexed> draws)
{
    auto commands=device.BeginCommands();Check(bool(commands),"PBR packet");Ok(commands->BeginPass({rhi::PassKind::Clear}),"PBR clear");
    Ok(commands->EndPass(),"PBR clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"PBR pass");
    for(const auto& draw:draws)Ok(commands->Draw(draw),"PBR three-slot draw");
    Ok(commands->EndPass(),"PBR end");Ok(commands->Close(),"PBR close");return commands;
}
rhi::Image Capture(Fixture& f,const rhi::DrawIndexed& draw)
{
    auto commands=Record(*f.device,std::span(&draw,1));rhi::CompletionToken token;rhi::Image image;
    Ok(f.device->Submit(*commands,token,&image),"PBR capture");return image;
}
rhi::Image Capture(Fixture& f,std::span<const rhi::DrawIndexed> draws)
{
    auto commands=Record(*f.device,draws);rhi::CompletionToken token;rhi::Image image;
    Ok(f.device->Submit(*commands,token,&image),"PBR capture");return image;
}
// 独立 double 精度 CPU oracle：逐通道求微表面项，不调用运行时着色器实现。
std::array<int,3> Reference(const rhi::DrawIndexed& draw,std::array<std::uint8_t,4> base,std::array<std::uint8_t,4> normal,std::array<std::uint8_t,4> mr)
{
    const auto& p=draw.pbr;auto n=Normalize(TransformVector(Transpose(*Inverse(draw.world)),{0,0,1}));
    const Vec3 origin=TransformPoint(draw.world,{});const float x=(160.5f/320*4)-2,y=1.5f-(120.5f/240*3);
    const Vec3 position{x,y,origin.z-(n.x*(x-origin.x)+n.y*(y-origin.y))/n.z};
    if(p.normalMapping&&std::abs(p.normalScale)>1e-8f)
    {
        auto t=TransformVector(draw.world,{1,0,0});t=Normalize(t-n*Dot(t,n));
        const float sign=Dot(Cross(TransformVector(draw.world,{1,0,0}),TransformVector(draw.world,{0,1,0})),TransformVector(draw.world,{0,0,1}))<0?-1.0f:1.0f;
        auto b=Cross(n,t)*sign;Vec3 mapped{(float(normal[0])/255*2-1)*p.normalScale,(float(normal[1])/255*2-1)*p.normalScale,float(normal[2])/255*2-1};
        mapped=Normalize(mapped);n=Normalize(t*mapped.x+b*mapped.y+n*mapped.z);
    }
    if(n.z<0&&p.doubleSided)n=-n;
    const auto v=Normalize(p.cameraPosition-position),l=Normalize(p.directionToLight),h=Normalize(v+l);
    const double nl=std::clamp(double(Dot(n,l)),0.0,1.0),nv=std::max(std::clamp(double(Dot(n,v)),0.0,1.0),1e-4);
    const double nh=std::clamp(double(Dot(n,h)),0.0,1.0),vh=std::clamp(double(Dot(v,h)),0.0,1.0);
    const double rough=std::max(0.045,double(p.roughness)*mr[1]/255),metal=double(p.metallic)*mr[2]/255;
    const double a2=std::pow(rough,4),denominator=nh*nh*(a2-1)+1;
    const double d=a2/std::max(3.141592653589793*denominator*denominator,1e-12);
    const double visibility=0.5/std::max(nl*std::sqrt(nv*nv*(1-a2)+a2)+nv*std::sqrt(nl*nl*(1-a2)+a2),1e-8);
    const float factors[]{draw.tint.x,draw.tint.y,draw.tint.z},lights[]{p.radiance.x,p.radiance.y,p.radiance.z};std::array<int,3> result{};
    for(size_t channel=0;channel<3;++channel)
    {
        double c=double(base[channel])/255;c=(c<=0.04045?c/12.92:std::pow((c+0.055)/1.055,2.4))*factors[channel];
        const double f0=0.04*(1-metal)+c*metal,f=f0+(1-f0)*std::pow(1-vh,5);
        const double linear=std::clamp(draw.unlit?c:((1-f)*(1-metal)*c/3.141592653589793+d*visibility*f)*nl*lights[channel],0.0,1.0);
        const double encoded=linear<=0.0031308?linear*12.92:1.055*std::pow(linear,1.0/2.4)-0.055;
        result[channel]=static_cast<int>(std::lround(encoded*255));
    }
    return result;
}
void Pixel(const rhi::Image& image,std::array<int,3> expected)
{
    Check(image.width==320&&image.height==240,"PBR image size");const auto offset=(size_t(120)*320+160)*4;
    for(size_t channel=0;channel<3;++channel)if(std::abs(int(image.rgba8[offset+channel])-expected[channel])>2)
    {std::cerr<<"PBR channel "<<channel<<" actual="<<int(image.rgba8[offset+channel])<<" expected="<<expected[channel]<<'\n';Check(false,"GGX/TBN GPU pixel agrees with CPU reference");}
}
void Gpu(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;auto model=Quad(f);std::vector<rhi::DrawIndexed> draws;
    AppendPbrModelDraws(*model,{}, {},draws,*f.pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});Check(draws.size()==1,"PBR draw adapter");
    auto draw=draws[0];const std::array<std::uint8_t,4> base{128,64,220,255},normal{166,204,226,255},mr{17,128,192,255};
    auto baseTexture=Texture(device,base),normalTexture=Texture(device,normal),mrTexture=Texture(device,mr);
    draw.texture=*baseTexture;draw.normalTexture=*normalTexture;draw.metallicRoughnessTexture=*mrTexture;
    draw.tint={0.7f,0.5f,0.25f,1};draw.pbr.metallic=0.7f;draw.pbr.roughness=0.6f;
    auto image=Capture(f,draw);Pixel(image,Reference(draw,base,normal,mr));Save(image,output/"reference.ppm");
    draw.pbr.normalMapping=true;
    for(size_t index=0;index<3;++index)
    {
        draw.world=index==0?Mat4{}:Rotation(AxisAngle({0,1,0},0.35f)*AxisAngle({0,0,1},0.3f))*Scaling({index==2?-1.2f:1.2f,0.7f,1.5f});
        image=Capture(f,draw);Pixel(image,Reference(draw,base,normal,mr));Save(image,output/("normal-transform-"+std::to_string(index)+".ppm"));
    }
    draw.world={};draw.pbr.normalScale=0;auto zero=Capture(f,draw);draw.pbr.normalMapping=false;auto disabled=Capture(f,draw);
    Check(zero.rgba8==disabled.rgba8,"normal scale zero exactly restores geometric normal");
    draw.world=Rotation(AxisAngle({0,1,0},Pi));auto culled=Capture(f,draw);draw.pbr.doubleSided=true;
    image=Capture(f,draw);Pixel(image,Reference(draw,base,normal,mr));Check(image.rgba8!=culled.rgba8,"double-sided back face lit with flipped normal");
    draw.world={};draw.pbr.doubleSided=false;
    for(float rough:{0.0f,0.001f,0.045f,0.5f,1.0f})for(float metal:{0.0f,1.0f})
    {draw.pbr.roughness=rough;draw.pbr.metallic=metal;Pixel(Capture(f,draw),Reference(draw,base,normal,mr));}
    auto commands=device.BeginCommands();Ok(commands->BeginPass({rhi::PassKind::Clear}),"capacity clear");Ok(commands->EndPass(),"capacity clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"capacity opaque");
    Ok(commands->Draw(draw),"first tuple");Ok(commands->Draw(draw),"duplicate tuple uses three slots");auto second=draw;second.normalTexture=*mrTexture;Ok(commands->Draw(second),"different normal forms second tuple");
    auto third=draw;third.metallicRoughnessTexture=*baseTexture;Check(commands->Draw(third).error==rhi::Error::CapacityExceeded,"third tuple exceeds six visible slots before recording");
    Ok(commands->EndPass(),"capacity end");Ok(commands->Close(),"capacity close");rhi::CompletionToken token;Ok(device.Submit(*commands,token),"legal capacity prefix submitted");
    const auto before=device.Stats();for(int index=0;index<24;++index){auto packet=Record(device,std::span(&draw,1));Ok(device.Submit(*packet,token),"ordinary PBR frame");}
    Check(device.Stats().globalWaits==before.globalWaits&&device.Stats().pipelineCreations==before.pipelineCreations,"PBR frames do not flush or recreate PSOs");
    Ok(device.WaitIdle(),"PBR before gated three-texture ownership");
    auto transient=Texture(device,{210,90,240,255});auto retained=rhi::D3D12DiagnosticsAccess::Lifetime(device,*transient);draw.normalTexture=*transient;
    auto packet=Record(device,std::span(&draw,1));Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf()))),"PBR ownership gate");
    struct Gate
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;HANDLE event;std::atomic<bool> timedOut{false};std::thread worker;
        explicit Gate(Microsoft::WRL::ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"PBR gate event");worker=std::thread([this]{if(WaitForSingleObject(event,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});}
        void Release(){fence->Signal(1);SetEvent(event);}
        ~Gate(){Release();if(worker.joinable())worker.join();CloseHandle(event);}
    } release{gate};
    Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::Queue(device)->Wait(gate.Get(),1)),"hold PBR execution");
    Ok(device.Submit(*packet,token),"three-texture packet behind gate");transient.reset();device.Collect();
    Check(!device.IsComplete(token)&&!retained.expired()&&!release.timedOut,"normal texture retained by actual in-flight packet");
    release.Release();Ok(device.WaitIdle(),"finish gated PBR");device.Collect();Check(retained.expired(),"normal texture actually retires after Fence");
    Ok(device.WaitIdle(),"PBR complete");const auto stats=device.Stats();Check(stats.debugWarnings==0&&stats.debugErrors==0,"PBR GPU warnings/errors zero");
}
void Materials(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;
    const auto sources=output/"material-source";fs::create_directories(sources);
    fs::copy_file(GetExecutableDirectory()/"Resources/Materials/normal.png",sources/"normal.png",fs::copy_options::overwrite_existing);
    fs::copy_file(GetExecutableDirectory()/"Resources/Textures/checker.png",sources/"base.png",fs::copy_options::overwrite_existing);
    AssetStore assets(sources,output/"material-cache");std::vector<AssetDiagnostic> diagnostics;AssetId normal,base,id;
    Check(assets.RegisterSource("normal.png",{},normal,diagnostics)&&assets.RegisterSource("base.png",{},base,diagnostics),"material texture registration");
    MaterialDocument document;document.baseColorTexture=base;document.normalTexture=normal;document.normalScale=9;
    Check(assets.SaveMaterialSource("test.material.json",document,id,diagnostics),"save source normal scale without destructive clamp");
    auto first=f.cache.LoadMaterial(assets,id,device,diagnostics);
    Check(first&&first->normalScale==8&&assets.LoadMaterial(id,diagnostics)->normalScale==9,"adapter clamps scale and preserves source");
    Check(std::any_of(diagnostics.begin(),diagnostics.end(),[](const auto& d){return d.key=="Rendering.NormalScaleClamped"&&d.warning;}),"scale clamp is diagnosed");
    Check(f.cache.LoadMaterial(assets,id,device,diagnostics)==first,"unchanged material and image revisions reuse binding");
    std::ofstream(sources/"normal.png",std::ios::binary)<<"broken image";
    Check(f.cache.LoadMaterial(assets,id,device,diagnostics)==first,"broken image retains last valid GPU revision");
    fs::copy_file(sources/"base.png",sources/"normal.png",fs::copy_options::overwrite_existing);
    auto imageChanged=f.cache.LoadMaterial(assets,id,device,diagnostics);
    Check(imageChanged&&imageChanged!=first&&*imageChanged->baseColor==*imageChanged->normal,"image-only change invalidates binding and same pixels share one texture across semantics");
    std::ofstream(sources/"test.material.json")<<"invalid JSON";
    Check(f.cache.LoadMaterial(assets,id,device,diagnostics)==imageChanged,"bad material retains last valid revision");
    document.roughness=0.3f;document.normalScale=1;
    Check(assets.SaveMaterialSource("test.material.json",document,id,diagnostics),"repair material");auto repaired=f.cache.LoadMaterial(assets,id,device,diagnostics);
    Check(repaired&&repaired!=imageChanged&&repaired->roughness==0.3f,"repaired source publishes new binding");
    f.cache.Prune(nullptr,{});first.reset();imageChanged.reset();repaired.reset();Ok(device.WaitIdle(),"material revisions complete");device.Collect();
    RECT rect{0,0,960,720};AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(f.window.hwnd,GWL_STYLE)),FALSE,0);
    Check(SetWindowPos(f.window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"material array window size");
    int exit=0;Check(PumpWindowMessages(exit),"material array window events");Ok(device.Resize(960,720),"material array targets");
    MaterialsDemo demo("en-US");auto services=demo.Services();Check(services.windowOpened(f.window.hwnd)&&services.update(0),"real Materials world setup");
    std::shared_ptr<const StaticRenderModel> sphere,cube;
    for(int primitive=0;primitive<2;++primitive)
    {
        auto source=std::make_shared<StaticModel>();source->meshes.push_back(primitive==0?MakeUnitSphere():MakeUnitCube());source->meshes[0].material=0;source->materials.emplace_back();
        ModelNode node;node.name="Primitive";node.meshes={0};source->nodes.push_back(node);
        Check(f.cache.Upload(source,{},sources,device,primitive==0?sphere:cube,diagnostics),"shared sample primitive upload");
    }
    Ok(device.EnableGpuTiming(true),"PBR sample timestamp enable");
    const auto render=[&](std::uint64_t frame,bool capture){
        FrameData data;services.extract(data);Check(data.meshes.size()==28&&data.lights.size()==1,"Materials uses actual World, 25 shared spheres and three cubes");
        auto selected=SelectCamera(data.cameras,{0,0,960,720},frame);Check(selected.snapshot.has_value(),"Materials camera snapshot");
        const auto& camera=*selected.snapshot;const auto& light=data.lights[0];std::vector<rhi::DrawIndexed> draws;std::vector<AssetId> needed;
        for(const auto& instance:data.meshes)
        {
            std::vector<std::shared_ptr<const PbrRenderMaterial>> overrides;
            for(const auto& materialId:instance.mesh.materials){needed.push_back(materialId);overrides.push_back(f.cache.LoadMaterial(*data.assets,materialId,device,diagnostics));Check(bool(overrides.back()),"sample override material loaded");}
            AppendPbrModelDraws(instance.mesh.meshAsset==BuiltinSphereAssetId?*sphere:*cube,instance.world,overrides,draws,*f.pipeline,camera.viewProjection,camera.position,
                Normalize(TransformVector(light.world,{0,0,1})),light.light.color*light.light.intensity);
        }
        Check(draws.size()==28,"all sample parts in one PBR pass");auto commands=Record(device,draws);f.cache.Prune(data.assets,{},needed);
        rhi::Image image;rhi::CompletionToken token;Ok(device.Submit(*commands,token,capture?&image:nullptr),"Materials frame submit");return image;
    };
    const auto mapped=render(1,true);Save(mapped,output/"materials.ppm");
    InputState input;input.hasFocus=true;input.keyEvents.push_back({KeyEventType::Pressed,'N'});services.input(input,false,false);Check(services.update(0),"normal toggle update");
    const auto geometric=render(2,true);Save(geometric,output/"materials-geometric.ppm");Check(mapped.rgba8!=geometric.rgba8,"actual sample normal toggle changes visible pixels");
    const auto before=device.Stats();for(std::uint64_t frame=3;frame<=18;++frame){Check(services.update(1.0/60),"material animation");render(frame,false);}
    Check(device.Stats().globalWaits==before.globalWaits&&device.Stats().uploadSubmissions==before.uploadSubmissions,"unchanged sample materials neither flush nor reupload");
    Ok(device.ExportGpuTimingsCsv(output/"materials-gpu.csv"),"PBR sample GPU timing export");
    Check(device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"Materials GPU warnings/errors zero");services.windowClosing();
}
// A01.02：材质引用用户 PBR 产物、内容修订与同帧默认/自定义混合。
void MaterialShader(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;
    const auto sources=output/"shader-material-source";fs::create_directories(sources);
    fs::copy_file(GetExecutableDirectory()/"Resources/Textures/checker.png",sources/"base.png",fs::copy_options::overwrite_existing);
    fs::copy_file(GetExecutableDirectory()/"Resources/Materials/normal.png",sources/"normal.png",fs::copy_options::overwrite_existing);
    AssetStore assets(sources,output/"shader-material-cache");std::vector<AssetDiagnostic> diagnostics;AssetId base,normal,shader;
    Check(assets.RegisterSource("base.png",{},base,diagnostics)&&assets.RegisterSource("normal.png",{},normal,diagnostics),"shader material textures");
    D3D12ShaderSource source;
    Check(ReadD3D12ShaderSource({rhi::PipelineKind::PbrMesh,GetExecutableDirectory()/"Resources/Shaders/Pbr.hlsl",rhi::PbrMaterialLayoutVersion},source,text),"fixed PBR source for user package");
    const auto program=CompileD3D12ShaderProgram(source,text);Check(bool(program),"compile fixed PBR source for user package");
    const auto artifact=sources/"user-pbr.sebin";Check(WriteD3D12UserPbrPackage(*program,artifact,text),"write user PBR package");
    Check(assets.RegisterSource("user-pbr.sebin",{},shader,diagnostics),"register user shader artifact");
    MaterialDocument document;document.baseColorTexture=base;document.normalTexture=normal;document.shader=shader;AssetId id;
    Check(assets.SaveMaterialSource("shader.material.json",document,id,diagnostics),"save material with shader reference");
    const auto loads=device.Stats().shaderArtifactLoads;
    const auto custom=f.cache.LoadMaterial(assets,id,device,diagnostics);
    Check(custom&&custom->pipeline&&device.Stats().shaderArtifactLoads==loads+1,"material revision loads user artifact once");
    Check(f.cache.LoadMaterial(assets,id,device,diagnostics)==custom&&device.Stats().shaderArtifactLoads==loads+1,"unchanged material and artifact reuse revision");
    const auto good=Read(artifact);
    // 有效内容修订：改写固定 PBR 像素输出后重新生成同一路径产物，必须按新内容准备候选。
    const auto variantSource=sources/"UserPbr.hlsl";
    {auto hlsl=Read(GetExecutableDirectory()/"Resources/Shaders/Pbr.hlsl");
     Replace(hlsl,"return float4(OutputSrgb(materialOptions.y>0.5?color:lit),1);","return float4(OutputSrgb(materialOptions.y>0.5?color:lit)*0.5,1);");
     std::ofstream output(variantSource,std::ios::binary);output<<hlsl;}
    fs::copy_file(GetExecutableDirectory()/"Resources/Shaders/PbrBindings.hlsli",sources/"PbrBindings.hlsli",fs::copy_options::overwrite_existing);
    D3D12ShaderSource variantShader;
    Check(ReadD3D12ShaderSource({rhi::PipelineKind::PbrMesh,variantSource,rhi::PbrMaterialLayoutVersion},variantShader,text),"compile patched PBR source");
    const auto variantProgram=CompileD3D12ShaderProgram(variantShader,text);Check(bool(variantProgram),"variant program");
    Check(WriteD3D12UserPbrPackage(*variantProgram,artifact,text),"rewrite user package with new pixel code");
    const auto revised=f.cache.LoadMaterial(assets,id,device,diagnostics);
    Check(revised&&revised!=custom&&revised->pipeline&&device.Stats().shaderArtifactLoads==loads+2,"changed artifact content prepares a new candidate pipeline");
    auto model=Quad(f);const auto viewProjection=VP();std::vector<rhi::DrawIndexed> draws;
    const Vec3 camera{0,0,3},lightDirection{0.4f,0.5f,1},radiance{1.5f,1.2f,0.9f};
    const auto capture=[&](const rhi::DrawIndexed& draw){auto packet=Record(device,std::span<const rhi::DrawIndexed>(&draw,1));rhi::CompletionToken token;rhi::Image image;Ok(device.Submit(*packet,token,&image),"single pipeline capture");return image;};
    std::vector<rhi::DrawIndexed> reference;
    std::vector<std::shared_ptr<const PbrRenderMaterial>> variantOverrides{revised};
    AppendPbrModelDraws(*model,{},variantOverrides,reference,*f.pipeline,viewProjection,camera,lightDirection,radiance);
    Check(reference.size()==1&&reference[0].pipeline==*revised->pipeline,"patched revision selects its own pipeline");
    const auto variantImage=capture(reference[0]);
    auto defaultDraw=reference[0];defaultDraw.pipeline=*f.pipeline;
    Check(capture(defaultDraw).rgba8!=variantImage.rgba8,"user pixel code visibly changes material pixels");
    {std::ofstream broken(artifact,std::ios::binary|std::ios::trunc);const char header[]="SESHDR1\0\0\0\0";broken.write(header,12);}
    diagnostics.clear();
    Check(f.cache.LoadMaterial(assets,id,device,diagnostics)==revised,"broken artifact keeps the last valid material revision");
    Check(std::any_of(diagnostics.begin(),diagnostics.end(),[](const auto& d){return d.key=="Material.ShaderPipelineFailed";}),"shader pipeline failure is diagnosed");
    {std::ofstream restore(artifact,std::ios::binary|std::ios::trunc);restore.write(good.data(),static_cast<std::streamsize>(good.size()));}
    const auto restored=f.cache.LoadMaterial(assets,id,device,diagnostics);
    Check(restored&&restored->pipeline,"restored artifact prepares a pipeline again");
    const std::shared_ptr<const PbrRenderMaterial> overrides[]{restored};
    AppendPbrModelDraws(*model,Translation({-0.5f,0,0})*Scaling({0.5f,1,1}),{},draws,*f.pipeline,viewProjection,camera,lightDirection,radiance);
    AppendPbrModelDraws(*model,Translation({0.5f,0,0})*Scaling({0.5f,1,1}),overrides,draws,*f.pipeline,viewProjection,camera,lightDirection,radiance);
    Check(draws.size()==2&&draws[0].pipeline==*f.pipeline&&draws[1].pipeline==*restored->pipeline,"append selects default and material pipelines per draw");
    const auto left=capture(draws[0]),right=capture(draws[1]);
    auto packet=Record(device,draws);rhi::CompletionToken token;rhi::Image mixed;
    Ok(device.Submit(*packet,token,&mixed),"mixed pipeline frame in one pass");
    size_t mismatches=0;
    for(UINT y=0;y<mixed.height;++y)for(UINT x=0;x<mixed.width;++x)
    {
        const auto offset=(static_cast<size_t>(y)*mixed.width+x)*4;const auto& expected=x<160?left:right;
        for(size_t channel=0;channel<4;++channel)if(mixed.rgba8[offset+channel]!=expected.rgba8[offset+channel])++mismatches;
    }
    Check(!mismatches,"mixed frame equals separate default and material pipeline captures");
    Check(device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"mixed pipeline GPU warnings/errors zero");
    Save(variantImage,output/"shader-material-user.ppm");Save(mixed,output/"shader-material-mixed.ppm");
}
// G04.04：同一张贴图分别走 RGBA8 与 BC1 派生路径，真实 PBR 采样逐像素对照；
// 另用 1x1 中灰块确认 sRGB 视图 0 与线性视图 1 的取值差异。
void Bc1Sampling(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;
    const auto root=output/"bc1-source";fs::create_directories(root);
    fs::copy_file(GetExecutableDirectory()/"Resources/Materials/normal.png",root/"base.png",fs::copy_options::overwrite_existing);
    fs::copy_file(GetExecutableDirectory()/"Resources/Materials/normal.png",root/"normal.png",fs::copy_options::overwrite_existing);
    auto model=std::make_shared<StaticModel>();
    MeshData mesh;mesh.name="Bc1Quad";mesh.material=0;
    mesh.vertices={{{-1,-1,0},{0,0,1},{0,1},{1,0,0,1}},{{1,-1,0},{0,0,1},{1,1},{1,0,0,1}},
        {{1,1,0},{0,0,1},{1,0},{1,0,0,1}},{{-1,1,0},{0,0,1},{0,0},{1,0,0,1}}};
    mesh.indices={0,1,2,0,2,3};model->meshes.push_back(mesh);
    MaterialData material;material.name="Bc1Material";material.metallic=0.2f;material.roughness=0.5f;
    material.baseColorTexture=0;material.normalTexture=1;material.metallicRoughnessTexture=1;
    model->materials.push_back(material);
    model->textures.push_back({"base.png",{}, {},false});
    model->textures.push_back({"normal.png",{}, {},false});
    ModelNode node;node.name="Quad";node.meshes={0};model->nodes.push_back(node);

    StaticModelRenderCache rgbaCache{true},colorOnlyCache{true},bc1Cache{true};
    colorOnlyCache.EnableBc1Textures(true,output/"bc1-color-store");
    bc1Cache.EnableBc1Textures(true,output/"bc1-store",true);
    std::vector<AssetDiagnostic> diagnostics;
    std::shared_ptr<const StaticRenderModel> rgbaModel,colorOnlyModel,bc1Model;
    Check(rgbaCache.Upload(model,root/"Bc1Quad.gltf",root,device,rgbaModel,diagnostics),"RGBA8 textured quad upload");
    Check(colorOnlyCache.Upload(model,root/"Bc1Quad.gltf",root,device,colorOnlyModel,diagnostics),"BC1 color textured quad upload");
    Check(bc1Cache.Upload(model,root/"Bc1Quad.gltf",root,device,bc1Model,diagnostics),"BC1 textured quad upload");
    Check(colorOnlyCache.Bc1TexturesEnabled()&&!colorOnlyCache.Bc1DataTexturesEnabled()
        &&colorOnlyCache.CompressedUploads()==1&&colorOnlyCache.BakedTextureCount()==1,
        "BC1 color-only policy keeps data textures RGBA8");
    Check(bc1Cache.Bc1DataTexturesEnabled()&&bc1Cache.CompressedUploads()==2&&bc1Cache.BakedTextureCount()==2
        &&bc1Cache.BakedTextureBytes()>0,"BC1 base color and data textures baked");
    Check(!rgbaCache.Bc1TexturesEnabled()&&rgbaCache.CompressedUploads()==0&&rgbaCache.BakedTextureCount()==0,
        "RGBA8 cache keeps compression disabled");

    // G04.05：BC5 法线变体管线（Shader 只采样 XY 并重建 Z）。
    rhi::Pipeline bc5Pipeline;
    {
        rhi::PipelineDescription description{rhi::PipelineKind::PbrMesh,GetExecutableDirectory()/"Resources/Shaders/Pbr.hlsl",2};
        description.macros.push_back({"SE_NORMAL_BC5","1"});
        Ok(device.CreatePipeline(description,bc5Pipeline),"BC5 normal PBR variant");
    }

    std::vector<rhi::DrawIndexed> draws;
    AppendPbrModelDraws(*rgbaModel,{}, {},draws,*f.pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
    const auto rgbaDraw=draws.back();draws.clear();
    AppendPbrModelDraws(*colorOnlyModel,{}, {},draws,*f.pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
    const auto baseOnlyDraw=draws.back();draws.clear();
    AppendPbrModelDraws(*bc1Model,{}, {},draws,*f.pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
    const auto bc1Draw=draws.back();
    const auto rgbaImage=Capture(f,rgbaDraw);
    const auto baseOnlyImage=Capture(f,baseOnlyDraw);
    auto dataOnlyDraw=rgbaDraw;dataOnlyDraw.normalTexture=bc1Draw.normalTexture;
    dataOnlyDraw.metallicRoughnessTexture=bc1Draw.metallicRoughnessTexture;
    const auto dataOnlyImage=Capture(f,dataOnlyDraw);
    const auto bc1Image=Capture(f,bc1Draw);
    Save(rgbaImage,output/"bc1-rgba8.ppm");Save(bc1Image,output/"bc1-sampled.ppm");
    Save(baseOnlyImage,output/"bc1-base-only.ppm");Save(dataOnlyImage,output/"bc1-data-only.ppm");
    struct DiffStats {std::uint64_t visible=0,changed=0;int maximum=0;double mean=0,psnr=0;};
    const auto compare=[&](const char* label,const rhi::Image& reference,const rhi::Image& candidate)
    {
        DiffStats stats;const int background[3]{reference.rgba8[0],reference.rgba8[1],reference.rgba8[2]};
        double total=0,errorSquared=0;
        for(std::size_t offset=0;offset<reference.rgba8.size();offset+=4)
        {
            if(reference.rgba8[offset]==background[0]&&reference.rgba8[offset+1]==background[1]
                &&reference.rgba8[offset+2]==background[2])continue;
            ++stats.visible;int local=0;
            for(std::size_t channel=0;channel<3;++channel)
            {
                const int difference=std::abs(int(reference.rgba8[offset+channel])-int(candidate.rgba8[offset+channel]));
                local=(std::max)(local,difference);total+=difference;
                const double error=double(reference.rgba8[offset+channel])-double(candidate.rgba8[offset+channel]);
                errorSquared+=error*error;
            }
            stats.maximum=(std::max)(stats.maximum,local);stats.changed+=local>2;
        }
        stats.mean=stats.visible?total/double(stats.visible*3):0;
        const double mse=stats.visible?errorSquared/double(stats.visible*3):0;
        stats.psnr=mse>0?10.0*std::log10(255.0*255.0/mse):99.0;
        std::cout<<"Texture "<<label<<": visible="<<stats.visible<<" changed_gt2="<<stats.changed
            <<" max="<<stats.maximum<<" mean="<<stats.mean<<" psnr="<<stats.psnr<<" dB\n";
        return stats;
    };
    const auto all=compare("bc1-all",rgbaImage,bc1Image);
    const auto baseOnly=compare("bc1-base-only",rgbaImage,baseOnlyImage);
    const auto dataOnly=compare("bc1-data-only",rgbaImage,dataOnlyImage);
    Check(all.visible>10000&&baseOnly.visible==all.visible&&dataOnly.visible==all.visible,
        "BC1 test quad covers the sampled region");
    Check(baseOnly.changed>0&&baseOnly.psnr>=29.0,"BC1 base color keeps PSNR near 30 dB");
    Check(all.changed>0&&dataOnly.changed>0,"BC1 sampling is lossy");
    // 法线/MR 用 BC1 只是 G04.04 的线性视图验证；BC5 法线在下面单独对照。
    Check(all.psnr>=18.0&&all.mean<20.0,"BC1 all-slots sampling stays within the documented quality bound");

    // G04.05：法线改 BC5（只存 XY，Shader 重建 Z），与 G04.04 的 BC1 法线对比。
    StaticModelRenderCache bc5Cache{true};
    bc5Cache.EnableBc1Textures(true,output/"bc5-store");
    bc5Cache.SetBc5Normals(true);
    std::shared_ptr<const StaticRenderModel> bc5Model;
    const auto beforeBc5Upload=device.Stats().uploadedBytes;
    Check(bc5Cache.Upload(model,root/"Bc1Quad.gltf",root,device,bc5Model,diagnostics),"BC5 normal textured quad upload");
    const auto bc5UploadBytes=device.Stats().uploadedBytes-beforeBc5Upload;
    Check(bc5Cache.Bc5NormalsEnabled()&&bc5Cache.CompressedUploads()==2&&bc5Cache.BakedTextureCount()==2,
        "BC5 normal replaces the compressed data texture");
    draws.clear();
    AppendPbrModelDraws(*bc5Model,{}, {},draws,*bc5Pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
    const auto bc5NormalImage=Capture(f,draws.back());
    Save(bc5NormalImage,output/"bc5-normal.ppm");
    const auto bc5Normal=compare("bc5-normal",rgbaImage,bc5NormalImage);
    Check(bc5Normal.psnr>=28.0&&bc5Normal.psnr>dataOnly.psnr+4.0,"BC5 normal improves over the BC1 data path");

    // G04.06：预算压到极小值后驻留降级并钳制 SRV，采样结果必须变化；重复更新不再降级。
    bc5Cache.SetTextureBudget(1);
    const auto degrade=bc5Cache.UpdateTextureResidency(device);
    Check(degrade.degraded>=1&&degrade.loadedBytes<=32&&degrade.budgetBytes==1,
        "residency degrades compressed textures under budget");
    for(const auto& [id,entry]:bc5Cache.Residency().Entries())
        Check(entry.desiredMip+1==entry.mipCount,"degraded textures keep only the smallest mip");
    draws.clear();
    AppendPbrModelDraws(*bc5Model,{}, {},draws,*bc5Pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
    const auto degradedImage=Capture(f,draws.back());
    Save(degradedImage,output/"residency-degraded.ppm");
    const auto degraded=compare("residency-degraded",rgbaImage,degradedImage);
    Check(degradedImage.rgba8!=bc5NormalImage.rgba8,"residency clamp changes sampling");
    Check(degraded.psnr<bc5Normal.psnr,"degraded mips reduce quality");
    const auto again=bc5Cache.UpdateTextureResidency(device);
    Check(again.degraded==0,"residency update is stable once inside budget");

    // G04.07：预算先压到 1 B 再上传，只创建最粗 mip；放宽预算后补传缺失 mip，画面必须恢复到完整链。
    StaticModelRenderCache streamCache{true};
    streamCache.EnableBc1Textures(true,output/"stream-store");
    streamCache.SetBc5Normals(true);
    streamCache.SetTextureBudget(1);
    std::shared_ptr<const StaticRenderModel> streamModel;
    const auto beforeStreamUpload=device.Stats().uploadedBytes;
    Check(streamCache.Upload(model,root/"Bc1Quad.gltf",root,device,streamModel,diagnostics),"streamed textured quad upload");
    const auto streamUploadBytes=device.Stats().uploadedBytes-beforeStreamUpload;
    // 完整 BC1(2,744)+BC5(5,488) 与只传最粗 mip(8+16) 的差；MR/几何两边相同，作差抵消。
    Check(bc5UploadBytes-streamUploadBytes==8208,"budget-limited creation skips the finer mip uploads");
    draws.clear();
    AppendPbrModelDraws(*streamModel,{}, {},draws,*bc5Pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
    const auto streamedImage=Capture(f,draws.back());
    Save(streamedImage,output/"residency-streamed.ppm");
    Check(streamedImage.rgba8==degradedImage.rgba8,"budget-limited creation samples only the smallest mips");
    for(const auto& [id,entry]:streamCache.Residency().Entries())
        Check(entry.desiredMip+1==entry.mipCount,"streamed textures start resident only at the smallest mip");
    const auto created=streamCache.UpdateTextureResidency(device);
    Check(created.degraded==2,"budget-limited creation reports two degraded textures");
    streamCache.SetTextureBudget(512ull*1024*1024);
    const auto beforeRestore=device.Stats().uploadedBytes;
    const auto restored=streamCache.UpdateTextureResidency(device);
    const auto restoreUploadBytes=device.Stats().uploadedBytes-beforeRestore;
    Check(restored.degraded==0,"raising the budget does not degrade");
    Check(restoreUploadBytes==8208,"restoring uploads exactly the missing mips");
    Check(streamCache.Residency().LoadedBytes()>1000,"raising the budget restores the full mip chain");
    draws.clear();
    AppendPbrModelDraws(*streamModel,{}, {},draws,*bc5Pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
    const auto restoredImage=Capture(f,draws.back());
    Save(restoredImage,output/"residency-restored.ppm");
    Check(restoredImage.rgba8==bc5NormalImage.rgba8,"streamed mips restore the full quality image exactly");

    // G04.08：单次补传预算 4 KiB，分多次完成；至少一帧应呈现中间质量。
    StaticModelRenderCache cappedCache{true};
    cappedCache.EnableBc1Textures(true,output/"capped-store");
    cappedCache.SetBc5Normals(true);
    cappedCache.SetTextureBudget(1);
    std::shared_ptr<const StaticRenderModel> cappedModel;
    Check(cappedCache.Upload(model,root/"Bc1Quad.gltf",root,device,cappedModel,diagnostics),"capped streaming upload");
    cappedCache.UpdateTextureResidency(device);
    cappedCache.SetTextureBudget(512ull*1024*1024);
    bool partial=false,finished=false;std::uint64_t totalUploaded=0;int calls=0;
    for(;calls<40&&!finished;++calls)
    {
        const auto update=cappedCache.UpdateTextureResidency(device,4096);
        Check(update.uploadedBytes<=4096,"capped upload respects the per-call budget");
        totalUploaded+=update.uploadedBytes;
        draws.clear();
        AppendPbrModelDraws(*cappedModel,{}, {},draws,*bc5Pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
        const auto image=Capture(f,draws.back());
        finished=image.rgba8==bc5NormalImage.rgba8;
        if(!finished&&image.rgba8!=streamedImage.rgba8)partial=true;
        if(!finished)Save(image,output/("residency-capped-"+std::to_string(calls)+".ppm"));
    }
    Check(finished,"capped streaming eventually restores the full image");
    Check(partial&&calls>1,"capped streaming shows intermediate quality across frames");
    Check(totalUploaded==8208,"capped streaming uploads exactly the missing mip bytes");

    auto quad=Quad(f);draws.clear();
    AppendPbrModelDraws(*quad,{}, {},draws,*f.pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
    auto plain=draws.back();
    const ImageData gray{1,1,{128,128,128,255}};BakedTexture grayBake;std::string bakeError;
    Check(BakeBc1Texture(gray,BakedTextureFormat::Bc1UnormSrgb,grayBake,bakeError)&&grayBake.mipLevels==1,
        "single block BC1 gray bake");
    rhi::Texture grayTexture;
    {
        rhi::TextureDescription description{1,1,1,1,rhi::TextureFormat::Bc1UnormSrgb,"Test.Bc1Gray"};
        const rhi::TextureSubresource subresource{std::span<const std::uint8_t>(grayBake.bytes),0};
        Ok(device.CreateTextureSubresources(description,std::span(&subresource,1),grayTexture),"BC1 gray upload");
    }
    plain.texture=*grayTexture;
    const std::array<std::uint8_t,4> decoded{132,130,132,255}; // 128 经 RGB565 端点往返后的块颜色
    const auto srgbImage=Capture(f,plain);
    Pixel(srgbImage,Reference(plain,decoded,{128,128,255,255},{255,255,255,255}));
    Save(srgbImage,output/"bc1-srgb-view.ppm");
    // PBR 受光材质禁止把 baseColor 当数据纹理：sRGB 视图 0 不能绕过。
    plain.textureIsData=true;
    auto packet=device.BeginCommands();Check(bool(packet),"BC1 view packet");
    Ok(packet->BeginPass({rhi::PassKind::Clear}),"BC1 view clear");Ok(packet->EndPass(),"BC1 view clear end");
    Ok(packet->BeginPass({rhi::PassKind::Opaque}),"BC1 view opaque");
    Check(packet->Draw(plain).error==rhi::Error::InvalidArgument,"PBR base color rejects the linear data view");
    Ok(packet->EndPass(),"BC1 view end");Ok(packet->Close(),"BC1 view close");
    rhi::CompletionToken rejectedToken;Ok(device.Submit(*packet,rejectedToken),"BC1 rejected view frame");
    // normal/MR 恒用线性视图 1：1x1 BC1 块解码后直接参与 CPU oracle，若错用 sRGB 视图像素必然不符。
    const ImageData normalPixels{1,1,{200,96,208,255}},mrPixels{1,1,{72,184,220,255}};
    auto bakeOne=[&](const ImageData& pixels,BakedTextureFormat format,std::array<std::uint8_t,4>& decodedBytes)
    {
        BakedTexture baked;std::string error;
        Check(BakeBc1Texture(pixels,format,baked,error)&&baked.mipLevels==1,"single block BC1 bake");
        Bc1Image block{1,1,baked.bytes};
        std::vector<std::uint8_t> rgba;DecodeBc1(block,rgba);
        Check(rgba.size()==4,"single block BC1 decode");
        decodedBytes={rgba[0],rgba[1],rgba[2],rgba[3]};
    };
    std::array<std::uint8_t,4> normalBytes{},mrBytes{};
    bakeOne(normalPixels,BakedTextureFormat::Bc1Unorm,normalBytes);
    bakeOne(mrPixels,BakedTextureFormat::Bc1Unorm,mrBytes);
    rhi::Texture normalTexture,mrTexture;
    {
        BakedTexture normalBake,mrBake;std::string error;
        Check(BakeBc1Texture(normalPixels,BakedTextureFormat::Bc1Unorm,normalBake,error)
            &&BakeBc1Texture(mrPixels,BakedTextureFormat::Bc1Unorm,mrBake,error),"BC1 normal and MR bakes");
        rhi::TextureDescription normalDescription{1,1,1,1,rhi::TextureFormat::Bc1Unorm,"Test.Bc1Normal"};
        rhi::TextureDescription mrDescription{1,1,1,1,rhi::TextureFormat::Bc1Unorm,"Test.Bc1Mr"};
        const rhi::TextureSubresource normalSub{std::span<const std::uint8_t>(normalBake.bytes),0};
        const rhi::TextureSubresource mrSub{std::span<const std::uint8_t>(mrBake.bytes),0};
        Ok(device.CreateTextureSubresources(normalDescription,std::span(&normalSub,1),normalTexture),"BC1 normal upload");
        Ok(device.CreateTextureSubresources(mrDescription,std::span(&mrSub,1),mrTexture),"BC1 MR upload");
    }
    plain.textureIsData=false;plain.normalTexture=*normalTexture;plain.metallicRoughnessTexture=*mrTexture;
    plain.pbr.normalMapping=true;plain.pbr.normalScale=1;
    const auto linearViewImage=Capture(f,plain);
    Pixel(linearViewImage,Reference(plain,decoded,normalBytes,mrBytes));
    Save(linearViewImage,output/"bc1-linear-view.ppm");

    // G04.05：1x1 BC5 法线（RG=128,128 → 重建 Z≈1）走 BC5 变体管线后与线性 oracle 一致。
    const ImageData bc5NormalPixels{1,1,{128,128,255,255}};
    BakedTexture bc5NormalBake;std::string bc5Error;
    Check(BakeBc5Texture(bc5NormalPixels,bc5NormalBake,bc5Error)&&bc5NormalBake.mipLevels==1,"single block BC5 bake");
    rhi::Texture bc5NormalTexture;
    {
        rhi::TextureDescription description{1,1,1,1,rhi::TextureFormat::Bc5Unorm,"Test.Bc5Normal"};
        const rhi::TextureSubresource subresource{std::span<const std::uint8_t>(bc5NormalBake.bytes),0};
        Ok(device.CreateTextureSubresources(description,std::span(&subresource,1),bc5NormalTexture),"BC5 normal upload");
    }
    auto bc5Plain=plain;bc5Plain.pipeline=*bc5Pipeline;bc5Plain.normalTexture=*bc5NormalTexture;
    const auto bc5OracleImage=Capture(f,bc5Plain);
    Pixel(bc5OracleImage,Reference(bc5Plain,decoded,{128,128,255,255},mrBytes));
    Save(bc5OracleImage,output/"bc5-normal-view.ppm");
    Check(device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"compressed sampling GPU warnings/errors zero");
}
// G05.06：同一相机下逐级绘制并与 LOD0 做像素对照（验证按级索引与几何简化）。
void LodSampling(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;
    auto source=std::make_shared<StaticModel>();
    source->meshes.push_back(MakeUnitSphere());
    source->materials.emplace_back();
    ModelNode node;node.name="Sphere";node.meshes={0};source->nodes.push_back(node);
    std::shared_ptr<const StaticRenderModel> model;std::vector<AssetDiagnostic> diagnostics;
    Check(f.cache.Upload(source,{},fs::current_path(),device,model,diagnostics,false,4),"LOD sphere upload");
    Check(model->meshes[0].lod&&model->meshes[0].lodIndices.size()==3,"LOD sphere index buffers");
    std::array<rhi::Image,4> images{};
    for(std::uint32_t level=0;level<4;++level)
    {
        std::vector<rhi::DrawIndexed> draws;
        AppendPbrModelDraws(*model,{}, {},draws,*f.pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f},level);
        Check(draws.size()==1&&draws[0].indexCount==model->meshes[0].lod->levels[level].indices.size(),
            "LOD draw uses the level index count");
        images[level]=Capture(f,draws[0]);
        Save(images[level],output/("lod-"+std::to_string(level)+".ppm"));
    }
    for(std::uint32_t level=1;level<4;++level)
    {
        std::uint64_t changed=0;int maximum=0;double squared=0;
        for(std::size_t offset=0;offset<images[0].rgba8.size();offset+=4)
        {
            int local=0;
            for(std::size_t channel=0;channel<3;++channel)
            {
                const int difference=std::abs(int(images[0].rgba8[offset+channel])-int(images[level].rgba8[offset+channel]));
                local=(std::max)(local,difference);
                const double error=double(images[0].rgba8[offset+channel])-double(images[level].rgba8[offset+channel]);
                squared+=error*error;
            }
            changed+=local>2;maximum=(std::max)(maximum,local);
        }
        const double mse=squared/double(images[0].width*images[0].height*3);
        const double psnr=mse>0?10.0*std::log10(255.0*255.0/mse):99.0;
        std::cout<<"LOD "<<level<<" vs LOD0: changed_gt2="<<changed<<" max="<<maximum<<" psnr="<<psnr<<" dB\n";
        Check(changed>0,"coarser LOD differs from LOD0");
    }
    Check(device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"LOD sampling GPU warnings/errors zero");
}
// G05.08：像素级 LOD dither。旧级画红、新级画绿，用 CPU 4×4 Bayer 预测逐像素结果；
// 只有 Shader 的阈值和分组与 CPU 完全一致，合成图才会与两次绘制的组合逐字节相同。
void LodDither(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;
    auto source=std::make_shared<StaticModel>();
    source->meshes.push_back(MakeUnitSphere());
    source->materials.emplace_back();
    ModelNode node;node.name="Sphere";node.meshes={0};source->nodes.push_back(node);
    std::shared_ptr<const StaticRenderModel> model;std::vector<AssetDiagnostic> diagnostics;
    Check(f.cache.Upload(source,{},fs::current_path(),device,model,diagnostics,false,4),"dither LOD sphere upload");
    const auto makeDraws=[&](std::uint32_t level,const Vec4& tint,float dither)
    {
        std::vector<rhi::DrawIndexed> draws;
        AppendPbrModelDraws(*model,{}, {},draws,*f.pipeline,VP(),{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f},level,dither);
        Check(draws.size()==1,"dither single draw");
        draws[0].unlit=true;draws[0].tint=tint;
        return draws;
    };
    const Vec4 red{1,0,0,1},green{0,1,0,1};
    auto oldDraws=makeDraws(0,red,0),newDraws=makeDraws(1,green,0);
    Check(oldDraws[0].pbr.lodDither==0.0f&&newDraws[0].pbr.lodDither==0.0f,"disabled dither defaults to zero");
    const auto background=Capture(f,std::span<const rhi::DrawIndexed>());
    const auto oldOnly=Capture(f,std::span<const rhi::DrawIndexed>(oldDraws));
    const auto newOnly=Capture(f,std::span<const rhi::DrawIndexed>(newDraws));
    Save(oldOnly,output/"dither-old.ppm");Save(newOnly,output/"dither-new.ppm");Save(background,output/"dither-bg.ppm");
    const auto same=[&](const rhi::Image& a,const rhi::Image& b,std::size_t offset)
    {return a.rgba8[offset]==b.rgba8[offset]&&a.rgba8[offset+1]==b.rgba8[offset+1]&&a.rgba8[offset+2]==b.rgba8[offset+2];};
    for(const float blend:{0.25f,0.5f})
    {
        auto draws=makeDraws(0,red,-blend);auto newer=makeDraws(1,green,blend);
        Check(draws[0].pbr.lodDither==-blend&&newer[0].pbr.lodDither==blend,"dither draw carries signed blend");
        draws.push_back(newer[0]);
        const auto mixed=Capture(f,std::span<const rhi::DrawIndexed>(draws));
        std::size_t mismatches=0,oldPixels=0,newPixels=0;
        for(std::size_t offset=0;offset<mixed.rgba8.size();offset+=4)
        {
            const auto pixel=offset/4;
            const auto x=static_cast<std::uint32_t>(pixel%mixed.width),y=static_cast<std::uint32_t>(pixel/mixed.width);
            const bool oldCovered=!same(oldOnly,background,offset),newCovered=!same(newOnly,background,offset);
            const bool usesPrevious=LodDitherUsesPrevious(blend,x,y);
            const auto& expected=usesPrevious?(oldCovered?oldOnly:background):(newCovered?newOnly:background);
            mismatches+=!same(mixed,expected,offset);
            if(oldCovered&&newCovered){oldPixels+=usesPrevious;newPixels+=!usesPrevious;}
        }
        const auto ratio=float(newPixels)/float(oldPixels+newPixels);
        std::cout<<"LOD dither blend="<<blend<<": mismatches="<<mismatches<<" old="<<oldPixels<<" new="<<newPixels
            <<" new_ratio="<<ratio<<"\n";
        Check(mismatches==0,"GPU Bayer dither matches the CPU 4x4 pattern exactly");
        Check(oldPixels>0&&newPixels>0&&std::abs(ratio-blend)<0.05f,"dither mix follows the blend weight");
        Save(mixed,output/("dither-blend-"+std::to_string(static_cast<int>(blend*100))+".ppm"));
    }
    // blend 1 时引擎只画新级（不带 dither），必须与单级图像逐字节一致。
    const auto full=Capture(f,std::span<const rhi::DrawIndexed>(newDraws));
    Check(full.rgba8==newOnly.rgba8,"full blend keeps the new level unchanged");
    Check(device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"LOD dither GPU warnings/errors zero");
}
// G05.09：HLOD 单元边界。8 个世界空间四边形横跨 x=0 的单元边界，
// 逐个实例绘制与“每单元一份合并代理”必须逐字节一致；混合模式证明单元可独立切换。
void HlodBoundary(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;
    MeshData quad;quad.name="HlodQuad";
    quad.vertices={{{-0.5f,-0.5f,0},{0,0,1},{0,1},{1,0,0,1}},{{0.5f,-0.5f,0},{0,0,1},{1,1},{1,0,0,1}},
        {{0.5f,0.5f,0},{0,0,1},{1,0},{1,0,0,1}},{{-0.5f,0.5f,0},{0,0,1},{0,0},{1,0,0,1}}};
    quad.indices={0,1,2,0,2,3};
    std::vector<Vec3> positions;
    for(const float x:{-3.5f,-2.5f,-1.5f,-0.5f,0.5f,1.5f,2.5f,3.5f})positions.push_back({x,0,0});
    std::vector<HlodInstance> instances;std::vector<MeshData> flatMeshes;
    for(const auto& position:positions)
    {
        const auto world=Translation(position);
        MeshData flat;TransformMeshToWorld(quad,world,flat);flatMeshes.push_back(std::move(flat));
        instances.push_back({&quad,nullptr,world,0,{}});
    }
    const auto upload=[&](std::vector<MeshData> meshes,const char* name)
    {
        auto model=std::make_shared<StaticModel>();
        for(auto& mesh:meshes){mesh.material=0;model->meshes.push_back(std::move(mesh));}
        model->materials.emplace_back();
        ModelNode node;node.name=name;for(std::uint32_t index=0;index<model->meshes.size();++index)node.meshes.push_back(index);
        model->nodes.push_back(std::move(node));
        std::shared_ptr<const StaticRenderModel> gpu;std::vector<AssetDiagnostic> diagnostics;
        Check(f.cache.Upload(model,{},fs::current_path(),device,gpu,diagnostics),"HLOD model upload");
        return gpu;
    };
    const auto individual=upload(flatMeshes,"HLODIndividual");
    HlodScene scene;std::vector<AssetDiagnostic> diagnostics;
    Check(BakeHlod(instances,64.0f,scene,diagnostics),"HLOD boundary bake");
    Check(scene.cells.size()==2&&scene.cells[0].coordinate.x==-1&&scene.cells[1].coordinate.x==0,"HLOD boundary cells");
    Check(scene.cells[0].parts.size()==1&&scene.cells[0].parts[0].mesh.indices.size()/3==8,"HLOD left cell merges four quads");
    Check(scene.cells[1].parts.size()==1&&scene.cells[1].parts[0].mesh.indices.size()/3==8,"HLOD right cell merges four quads");
    std::vector<std::shared_ptr<const StaticRenderModel>> proxies;
    for(auto& cell:scene.cells)proxies.push_back(upload({std::move(cell.parts[0].mesh)},"HLODProxy"));
    const auto hlodVp=*OrthographicRH(8,4,0.1f,100)*Translation({0,0,-3});
    const auto draw=[&](const std::shared_ptr<const StaticRenderModel>& model,std::vector<rhi::DrawIndexed>& draws)
    {
        AppendPbrModelDraws(*model,{}, {},draws,*f.pipeline,hlodVp,{0,0,3},{0.4f,0.5f,1},{1.5f,1.2f,0.9f});
        for(auto& item:draws){item.unlit=true;item.tint={0,1,0,1};}
    };
    std::vector<rhi::DrawIndexed> individualDraws,proxyDraws;
    draw(individual,individualDraws);
    for(const auto& proxy:proxies)draw(proxy,proxyDraws);
    Check(individualDraws.size()==8&&proxyDraws.size()==2,"HLOD draw counts");
    std::uint64_t individualTriangles=0,proxyTriangles=0;
    for(const auto& item:individualDraws)individualTriangles+=item.indexCount/3;
    for(const auto& item:proxyDraws)proxyTriangles+=item.indexCount/3;
    Check(individualTriangles==16&&proxyTriangles==16,"HLOD proxies cover every triangle exactly once at the boundary");
    const auto individuals=Capture(f,std::span<const rhi::DrawIndexed>(individualDraws));
    const auto merged=Capture(f,std::span<const rhi::DrawIndexed>(proxyDraws));
    Save(individuals,output/"hlod-individual.ppm");Save(merged,output/"hlod-proxy.ppm");
    Check(individuals.rgba8==merged.rgba8,"HLOD cell proxies match individual draws pixel-for-pixel");
    // 混合：左侧单元保持实例、右侧单元用代理，相邻单元切换时机不同也不产生空洞/重叠。
    std::vector<MeshData> leftMeshes;
    for(std::size_t index=0;index<positions.size();++index)if(positions[index].x<0)leftMeshes.push_back(flatMeshes[index]);
    const auto left=upload(std::move(leftMeshes),"HLODLeftIndividual");
    std::vector<rhi::DrawIndexed> mixedDraws;
    draw(left,mixedDraws);draw(proxies[1],mixedDraws);
    Check(mixedDraws.size()==5,"HLOD mixed mode draws one cell plus four instances");
    const auto mixed=Capture(f,std::span<const rhi::DrawIndexed>(mixedDraws));
    Save(mixed,output/"hlod-mixed.ppm");
    Check(mixed.rgba8==individuals.rgba8,"independent cell switching keeps the boundary intact");
    std::cout<<"HLOD boundary: individual_draws="<<individualDraws.size()<<", proxy_draws="<<proxyDraws.size()
        <<", triangles="<<individualTriangles<<"/"<<proxyTriangles<<", pixels="
        <<individuals.width*individuals.height<<", identical=1\n";
    Check(device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"HLOD boundary GPU warnings/errors zero");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply PBR test output");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"PBR strings");
        Cpu(output);Gpu(text,output);Materials(text,output);MaterialShader(text,output);Bc1Sampling(text,output);LodSampling(text,output);LodDither(text,output);HlodBoundary(text,output);std::cout<<"PbrTests passed (sphere, normalScale, GGX oracle, sRGB/data slots, TBN transforms, material tables, revision recovery, user shader material revision, mixed pipelines, BC1 sampling, LOD levels, LOD dither, HLOD boundary, Fence lifetime and real World sample); GPU warnings/errors=0.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"PbrTests: "<<error.what()<<'\n';return 1;}
}
