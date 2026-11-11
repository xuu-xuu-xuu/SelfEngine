#include "FunctionLayer/Rendering/StaticModelRender.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Windows/Window.h"
#include "StaticModel/StaticModelDemo.h"
#include "Scene/SceneDemo.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool Near(float a,float b){return std::abs(a-b)<0.0001f;}
struct Fixture
{
    WindowState window;
    std::shared_ptr<se::rhi::Device> device;
    se::rhi::Pipeline pipeline;
    se::StaticModelRenderCache cache;
    ~Fixture(){if(device)device->Shutdown();cache.Clear();pipeline.reset();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
using Image=se::rhi::Image;
se::Mat4 VP(){return *se::PerspectiveRH(se::Pi/3,4.0f/3,0.1f,100)*se::Translation({-1,-0.8f,-4});}
Image Capture(Fixture& f,const se::StaticRenderModel& model,const Localization&,const se::Mat4& entity={})
{
    auto commands=f.device->BeginCommands();Check(bool(commands),"begin model RHI context");
    Check(bool(commands->BeginPass({se::rhi::PassKind::Clear})),"declare clear outputs");
    Check(bool(commands->EndPass())&&bool(commands->BeginPass({se::rhi::PassKind::Opaque})),"ordered opaque pass");
    std::vector<se::rhi::DrawIndexed> draws;se::AppendStaticModelDraws(model,entity,draws,*f.pipeline,VP(),true);
    for(const auto& draw:draws)Check(bool(commands->Draw(draw)),"bind imported model through RHI");
    Check(bool(commands->EndPass())&&bool(commands->Close()),"close model RHI context");
    Image image;se::rhi::CompletionToken token;
    Check(bool(f.device->Submit(*commands,token,&image))&&f.device->IsComplete(token),"capture imported model through RHI");
    return image;
}
void Save(const Image& image,const fs::path& path)
{
    std::ofstream file(path,std::ios::binary);file<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t offset=0;offset<image.rgba8.size();offset+=4)file.write(reinterpret_cast<const char*>(image.rgba8.data()+offset),3);
    Check(bool(file),"save model GPU image");
}
void Pixel(const Image& image,se::Vec3 point,std::array<int,3> expected)
{
    const auto clip=VP()*se::Vec4{point.x,point.y,point.z,1};
    const auto x=static_cast<UINT>((clip.x/clip.w*0.5f+0.5f)*image.width);
    const auto y=static_cast<UINT>((0.5f-clip.y/clip.w*0.5f)*image.height);
    Check(x<image.width&&y<image.height,"reference point in viewport");
    const auto offset=(size_t(y)*image.width+x)*4;
    for(size_t channel=0;channel<3;++channel)
        Check(std::abs(int(image.rgba8[offset+channel])-expected[channel])<=2,"known imported UV/material pixel");
}
void Bounds(const Image& image)
{
    UINT left=image.width,top=image.height,right=0,bottom=0,count=0;
    for(UINT y=0;y<image.height;++y)for(UINT x=0;x<image.width;++x)
    {
        const size_t offset=(size_t(y)*image.width+x)*4;
        if(image.rgba8[offset]>200||image.rgba8[offset+1]>200||image.rgba8[offset+2]>200)
        {left=std::min(left,x);top=std::min(top,y);right=std::max(right,x);bottom=std::max(bottom,y);++count;}
    }
    Check(count>2400&&count<2900,"one-meter by two-meter asymmetric triangle area");
    Check(left>=159&&left<=161&&right>=210&&right<=213&&top>=56&&top<=60&&bottom>=159&&bottom<=163,
        "parent x=1 meter applied exactly once on GPU");
}
void Cpu(const fs::path& root)
{
    std::vector<se::AssetDiagnostic> diagnostics;se::StaticModel input;
    Check(se::ImportStaticModel(root/"Hierarchy.glb",{},input,diagnostics),"import CPU fixture");
    const auto originalVertices=input.meshes[0].vertices;
    auto parent=*std::find_if(input.nodes.begin(),input.nodes.end(),[](const auto& n){return n.name=="Parent";});
    auto child=*std::find_if(input.nodes.begin(),input.nodes.end(),[](const auto& n){return n.name=="Triangle";});
    child.parent=1;child.localMatrix=se::Translation({0,0,0.1f}).values;
    parent.parent=-1;parent.localMatrix=(se::Translation({1,0,0})*se::Scaling({2,1,1})).values;
    input.nodes={child,parent};
    se::StaticModelLayout layout;
    Check(se::BuildStaticModelLayout(input,layout,diagnostics),"child before parent and nonuniform scale");
    Check(layout.parts.size()==1&&Near(layout.minimum.x,1)&&Near(layout.maximum.x,3)
        &&Near(layout.maximum.y,2)&&Near(layout.maximum.z,0.1f),"CPU model bounds from cumulative nodes");
    Check(input.meshes[0].vertices[1].position==originalVertices[1].position,"node not baked into geometry");
    const auto previous=layout.parts[0].modelFromMesh.values;
    input.nodes[1].parent=0;
    Check(!se::BuildStaticModelLayout(input,layout,diagnostics)&&layout.parts[0].modelFromMesh.values==previous,"cycle rejected transactionally");
    input.nodes[1].parent=-1;input.nodes[0].localMatrix=se::Scaling({0,1,1}).values;
    Check(!se::BuildStaticModelLayout(input,layout,diagnostics),"singular mesh matrix rejected");
    input.nodes[0].localMatrix=se::Mat4{}.values;
    input.materials[0].baseColor[0]=2;
    Check(!se::BuildStaticModelLayout(input,layout,diagnostics),"invalid material color rejected");
}
void Gpu(const fs::path& source,const fs::path& output,const Localization& text)
{
    Fixture f;Check(CreateEngineWindow(f.window,false),"create model hidden window");
    RECT rect{0,0,320,240};AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(f.window.hwnd,GWL_STYLE)),FALSE,0);
    Check(SetWindowPos(f.window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"size model window");
    int code=0;Check(PumpWindowMessages(code),"pump model window");
    se::rhi::Result result;f.device=se::rhi::Device::CreateD3D12({f.window.hwnd,320,240},text,result);
    Check(bool(f.device)&&bool(result),"initialize model RHI device");
    Check(bool(f.device->CreatePipeline({se::rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},f.pipeline)),"model RHI pipeline");
    se::AssetStore assets(source,output/"cache");std::vector<se::AssetDiagnostic> diagnostics;
    se::AssetId glbId,fbxId;
    Check(assets.RegisterSource("Hierarchy.glb",{},glbId,diagnostics)&&assets.RegisterSource("Hierarchy.fbx",{},fbxId,diagnostics),"register actual sample IDs");
    const auto glb=f.cache.Load(assets,glbId,*f.device,diagnostics);
    const auto fbx=f.cache.Load(assets,fbxId,*f.device,diagnostics);
    Check(glb&&fbx&&glb->state==se::ModelRenderState::Ready&&fbx->state==se::ModelRenderState::Ready,"FBX and GLB ready");
    Check(*glb->meshes[0].texture==*fbx->meshes[0].texture&&f.cache.TextureCount()==1,"cross-model texture and SRVs shared");
    const auto glbImage=Capture(f,*glb,text),fbxImage=Capture(f,*fbx,text);
    Bounds(glbImage);Bounds(fbxImage);
    Check(glbImage.rgba8==fbxImage.rgba8,"FBX and GLB identical geometry and UV output");
    Pixel(glbImage,{1.2f,0.5f,0},{255,255,255});Pixel(glbImage,{1.7f,0.2f,0},{255,0,0});Pixel(glbImage,{1.1f,1.2f,0},{0,0,255});
    Save(glbImage,output/"glb.ppm");Save(fbxImage,output/"fbx.ppm");
    const auto waits=f.device->Stats().globalWaits;
    for(int frame=0;frame<12;++frame)Check(f.cache.Load(assets,glbId,*f.device,diagnostics)==glb,"same model ID has one GPU revision");
    Check(f.device->Stats().globalWaits==waits&&f.cache.ModelCount()==2,"repeat requests cause no upload");

    auto multi=std::make_shared<se::StaticModel>(*glb->source);
    multi->meshes.push_back(multi->meshes[0]);multi->materials.push_back(multi->materials[0]);
    multi->materials[1].baseColor={0.25f,0.5f,1,1};multi->meshes[1].material=1;
    multi->textures.push_back(multi->textures[0]);multi->materials[1].baseColorTexture=1;
    se::ModelNode second;second.name="Second";second.localMatrix=se::Translation({0.6f,-0.4f,-0.2f}).values;second.meshes={1};multi->nodes.push_back(second);
    std::shared_ptr<const se::StaticRenderModel> uploaded;
    Check(f.cache.Upload(multi,source/"Hierarchy.glb",source,*f.device,uploaded,diagnostics),"multi mesh/material upload");
    Check(uploaded->meshes.size()==2&&*uploaded->meshes[0].texture==*uploaded->meshes[1].texture,"duplicate external references share GPU texture");
    std::vector<se::rhi::DrawIndexed> draws;
    const auto entity=se::Translation({-0.3f,0.1f,0})*se::Scaling({0.8f,1.2f,1});
    se::AppendStaticModelDraws(*uploaded,entity,draws,*f.pipeline,VP(),true);
    Check(draws.size()==2&&Near(draws[0].world(0,3),0.5f)&&Near(draws[1].world(0,3),0.18f)
        &&Near(draws[1].tint.x,0.25f),"entity*node order and separate material");
    const auto multiImage=Capture(f,*uploaded,text);Pixel(multiImage,{0.8f,0.1f,-0.2f},{137,188,255});
    Save(multiImage,output/"multi-material.ppm");
    std::vector<std::uint8_t> png;Check(se::asset_io::Read(source/"checker.png",png),"read embedded PNG");
    auto embedded=std::make_shared<se::StaticModel>(*glb->source);embedded->textures[0]={{},"image/png",png,false};
    std::shared_ptr<const se::StaticRenderModel> embeddedGpu;
    Check(f.cache.Upload(embedded,source/"embedded.glb",source,*f.device,embeddedGpu,diagnostics)
        &&*embeddedGpu->meshes[0].texture==*glb->meshes[0].texture,"embedded and external pixels share GPU storage");
    auto broken=std::make_shared<se::StaticModel>(*glb->source);broken->textures[0].relativePath="../outside.png";
    const auto old=uploaded;
    Check(!f.cache.Upload(broken,source/"broken.glb",source,*f.device,uploaded,diagnostics)&&uploaded==old,"path escape rejects and preserves prior output");
    broken->textures[0].relativePath="absent.png";
    Check(f.cache.Upload(broken,source/"missing.glb",source,*f.device,uploaded,diagnostics)
        &&uploaded->state==se::ModelRenderState::Fallback,"missing texture explicit fallback state");
    const auto fallback=Capture(f,*uploaded,text);Pixel(fallback,{1.2f,0.5f,0},{255,0,255});Save(fallback,output/"fallback.ppm");
    broken->textures[0].relativePath="bad.png";std::ofstream(source/"bad.png")<<"not PNG";
    Check(f.cache.Upload(broken,source/"corrupt.glb",source,*f.device,uploaded,diagnostics)
        &&uploaded->state==se::ModelRenderState::Fallback,"corrupt image explicit fallback");
    broken->textures.clear();broken->materials[0].baseColorTexture=-1;broken->materials[0].baseColor={0.25f,0.5f,1,1};
    Check(f.cache.Upload(broken,source/"plain.glb",source,*f.device,uploaded,diagnostics)
        &&uploaded->state==se::ModelRenderState::Ready,"unassigned texture uses white without error state");
    Pixel(Capture(f,*uploaded,text),{1.2f,0.5f,0},{137,188,255});
    std::ofstream(source/"checker.png",std::ios::binary)<<"damaged";
    Check(f.cache.Load(assets,glbId,*f.device,diagnostics)==glb,"failed assigned texture revision preserves ready model");
    const auto failedWaits=f.device->Stats().globalWaits;
    for(int attempt=0;attempt<5;++attempt)
        Check(f.cache.Load(assets,glbId,*f.device,diagnostics)==glb,"same failed revision retains old model");
    Check(f.device->Stats().globalWaits==failedWaits,"same failed revision does not re-upload every frame");
    std::ofstream(source/"Hierarchy.glb",std::ios::binary)<<"damaged";
    Check(f.cache.Load(assets,glbId,*f.device,diagnostics)==glb,"failed model reload preserves ready model");
    Check(f.cache.Load(assets,"11111111111111111111111111111111",*f.device,diagnostics)==nullptr,"missing first model diagnosed");
    const auto stats=f.device->Stats();
    Check(stats.debugWarnings==0&&stats.debugErrors==0,"GPU model warning/error/corruption zero");
    std::cout<<"Model RHI GPU InfoQueue warning/error/corruption=0\n";
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==3,"supply source and output paths");const auto source=fs::absolute(argv[1]);
        const auto output=fs::absolute(argv[2]);fs::create_directories(output);
        const auto scratch=output/"source"/se::asset_io::NewId();fs::create_directories(scratch);
        for(const auto& entry:fs::directory_iterator(source))if(entry.is_regular_file())fs::copy_file(entry.path(),scratch/entry.path().filename());
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"load model test strings");
        Cpu(scratch);Gpu(scratch,output,text);
        for(const auto* format:{"glb","fbx"})
        {
            SandboxRenderer renderer(SandboxScene::Scene);se::StaticModelDemo demo(format);auto services=demo.Services();services.maximumFrames=5;
            Check(RunSandboxApp("en-US",renderer,false,&services)==0&&demo.Scene().Size()==4,"real static model sample/World/AssetStore host");
        }
        const auto sceneSamples=source.parent_path()/"Scenes";
        for(const auto* name:{"Basic.scene.json","MissingResource.scene.json"})
        {
            SandboxRenderer renderer(SandboxScene::Scene);se::SceneDemo demo(sceneSamples/name,sceneSamples,"en-US");
            auto services=demo.Services();services.maximumFrames=5;
            Check(RunSandboxApp("en-US",renderer,false,&services)==0&&demo.Scene().Size()>=3,"file-selected scene player including missing asset");
        }
        const auto scenePath=output/"transaction.scene.json";fs::copy_file(sceneSamples/"Basic.scene.json",scenePath,fs::copy_options::overwrite_existing);
        const auto playerAssets=output/"scene-assets"/se::asset_io::NewId();fs::create_directories(playerAssets);
        se::SceneDemo scene(scenePath,playerAssets,"en-US");Check(scene.Load(),"load candidate scene");
        std::string before;Check(se::SerializeScene(scene.Scene(),before,error),"capture player scene");
        std::ofstream(scenePath)<<"broken JSON";Check(!scene.Load(),"bad player scene fails");
        std::string after;Check(se::SerializeScene(scene.Scene(),after,error)&&before==after,"bad load preserves previous player world");
        fs::copy_file(sceneSamples/"Basic.scene.json",scenePath,fs::copy_options::overwrite_existing);
        std::ofstream(playerAssets/"bad.glb.asset.json")<<"broken metadata";Check(!scene.Load(),"bad asset index fails");
        Check(se::SerializeScene(scene.Scene(),after,error)&&before==after,"bad asset index preserves previous player world");
        std::cout<<"Static model layout, real GPU pixels, texture sharing, revision rollback and hosts passed.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"StaticModelRenderTests: "<<error.what()<<'\n';return 1;}
}
