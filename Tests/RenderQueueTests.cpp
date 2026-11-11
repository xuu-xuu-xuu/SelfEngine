#include "FunctionLayer/Rendering/RenderQueue.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Rendering/RenderTelemetry.h"
#include "FunctionLayer/Rendering/RenderExperiment.h"
#include "FunctionLayer/Rendering/MaterialVariants.h"
#include "FunctionLayer/Rendering/MaterialInstance.h"
#include "ResourceLayer/Assets/VertexCompression.h"
#include "FunctionLayer/Rendering/MeshOptimization.h"
#include "FunctionLayer/Rendering/TextureResidency.h"
#include "ResourceLayer/Assets/TextureCompression.h"
#include "ResourceLayer/Assets/TextureBake.h"
#include "ResourceLayer/Assets/LodBake.h"
#include "ResourceLayer/Assets/HlodBake.h"
#include "FunctionLayer/Rendering/LodSelect.h"
#include "FunctionLayer/Rendering/HlodSelect.h"
#include "FunctionLayer/Rendering/HlodStream.h"
#include "FunctionLayer/Rendering/HlodStreamer.h"
#include "FunctionLayer/Rendering/HlodProxyCache.h"
#include "ResourceLayer/Assets/MeshletBake.h"
#include "FunctionLayer/Rendering/CascadeShadow.h"
#include "FunctionLayer/Rendering/LightClustering.h"
#include "FunctionLayer/Rendering/AmbientOcclusion.h"
#include "FunctionLayer/Rendering/Atmosphere.h"
#include "FunctionLayer/Rendering/Reflection.h"
#include "FunctionLayer/Rendering/FrameGraph.h"
#include "FunctionLayer/Rendering/TemporalAA.h"
#include "FunctionLayer/Rendering/AutoExposure.h"
#include "FunctionLayer/Rendering/ColorGrading.h"
#include "FunctionLayer/Rendering/ColorLutFile.h"
#include "FunctionLayer/Rendering/OcclusionHud.h"
#include "CoreLayer/Math/Half.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <map>
#include <iterator>
#include <numeric>
#include "ResourceLayer/Assets/ImpostorBake.h"
#include "FunctionLayer/Rendering/HiZ.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "Benchmark/BenchmarkScene.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <iostream>
#include <fstream>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Cpu()
{
    rhi::DrawIndexed base;base.pipeline={1,3,2};base.texture={1,8,1};base.normalTexture={1,9,1};base.metallicRoughnessTexture={1,10,1};base.vertices={1,1,1};base.indices={1,2,1};
    const auto key=BuildOpaqueSortKey(base);
    for(int variant=0;variant<9;++variant)
    {
        auto changed=base;switch(variant){case 0:changed.pipeline.device=2;break;case 1:++changed.pipeline.generation;break;case 2:++changed.texture.index;break;case 3:changed.tint.x=0.7f;break;
            case 4:changed.pbr.roughness=0.4f;break;case 5:changed.pbr.normalMapping=true;break;case 6:++changed.vertices.generation;break;case 7:++changed.indices.index;break;default:changed.pbr.receivesShadow=false;break;}
        Check(BuildOpaqueSortKey(changed)!=key,"full device/generation, material and mesh identities participate in key");
    }
    std::vector<rhi::DrawIndexed> draws;
    for(int index=0;index<100;++index){auto draw=base;draw.texture.index=8+unsigned(index%4);draw.world=Translation({float(index),2,3});draws.push_back(draw);}
    SortOpaqueDraws(draws);for(size_t index=1;index<draws.size();++index)Check(BuildOpaqueSortKey(draws[index-1])<=BuildOpaqueSortKey(draws[index]),"opaque keys sorted");
    for(size_t index=0;index<draws.size();++index){const auto expected=float(index/25)+float(index%25)*4;Check(draws[index].world(0,3)==expected&&draws[index].world(1,3)==2,"equal-key input order and all instance parameters preserved");}
    std::vector<rhi::DrawIndexed> empty;SortOpaqueDraws(empty);Check(empty.empty(),"empty queue");
}
void OcclusionHudVisuals(const Localization& text)
{
    // G06.09：HUD 是只读计数 → UiVisual 的纯 CPU 转换；默认小窗口下不建立覆盖层。
    rhi::OcclusionCounters counters;counters.valid=true;counters.frame=7;
    counters.candidates=801;counters.visible=481;counters.culled=320;counters.occlusionRate=320.0f/801.0f;
    const auto visuals=BuildOcclusionHud(counters,text,640,360);
    Check(visuals.size()>=6,"occlusion HUD builds panel, labels and bars");
    bool candidates=false,visible=false,culled=false,rate=false;
    for(const auto& visual:visuals)
    {
        candidates=candidates||visual.text.find("801")!=std::string::npos;
        visible=visible||visual.text.find("481")!=std::string::npos;
        culled=culled||visual.text.find("320")!=std::string::npos;
        rate=rate||visual.text.find("%")!=std::string::npos;
    }
    Check(candidates&&visible&&culled&&rate,"occlusion HUD shows readback counts and rate");
    const auto bar=std::find_if(visuals.begin(),visuals.end(),[](const UiVisual& visual)
        {return visual.kind==UiKind::Panel&&visual.color.g>visual.color.r&&visual.rectangle.height<20;});
    Check(bar!=visuals.end()&&std::abs(bar->rectangle.width-(304.0f-20.0f)*481.0f/801.0f)<1.0f,
        "occlusion HUD visible bar follows GPU visible count");
    counters.valid=false;
    const auto pending=BuildOcclusionHud(counters,text,640,360);
    Check(pending.size()==3&&pending[2].text.find(text.GetText("Rendering.OcclusionHudPending"))!=std::string::npos,
        "occlusion HUD shows a pending state before the first readback");
    Check(BuildOcclusionHud(counters,text,180,100).empty(),"occlusion HUD stays out of tiny surfaces");
}
class ObservedRenderer final:public Renderer
{
public:
    SandboxRenderer delegate{SandboxScene::Benchmark};rhi::Statistics last;SandboxDrawStatistics draws;bool resize=false;
    bool Initialize(const RenderSurface& surface,const Localization& text) override{return delegate.Initialize(surface,text);}
    RenderResult Render(const RenderFrame& frame,const Localization& text) override
    {
        const auto result=delegate.Render(frame,text);last=delegate.Statistics();draws=delegate.DrawStatistics();
        Check(last.debugWarnings==0&&last.debugErrors==0,"queue GPU diagnostics clean");
        if(resize&&frame.updateIndex==1)
        {
            RECT rect{0,0,384,256};Check(AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(frame.surface.window,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(frame.surface.window,GWL_EXSTYLE)))!=FALSE,"queue resize rect");
            Check(SetWindowPos(frame.surface.window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"queue resize");
        }
        return result;
    }
    void Shutdown() noexcept override{delegate.Shutdown();}
};
void Gpu(const std::filesystem::path& output)
{
    for(bool resize:{false,true})
    {
        std::vector<std::uint8_t> expected;std::uint64_t baselineTables=0,baselineVertices=0;
        for(int mode=0;mode<4;++mode)
        {
            const bool sorting=mode>=2,cache=(mode%2)!=0;BenchmarkSession session;session.captureMode=true;session.config.kind=BenchmarkKind::Mixed;session.config.objects=192;
            session.config.width=320;session.config.height=240;session.config.visibility=BenchmarkVisibility::AllVisible;session.config.radiusMultiplier=5;session.config.sortOpaque=sorting;session.config.cacheOpaqueBindings=cache;
            BenchmarkScene scene(session);auto services=scene.Services();services.maximumFrames=5;
            const auto original=services.extract;std::uint64_t extracted=0;
            services.extract=[&](FrameData& frame){original(frame);if(resize&&++extracted>=3){frame.meshes[0].world=frame.meshes[0].world*Scaling({-1.2f,1.8f,0.7f});frame.meshes[0].mesh.meshAsset=BuiltinSphereAssetId;frame.meshes[0].mesh.materials=frame.meshes[17].mesh.materials;}};
            ObservedRenderer renderer;renderer.resize=resize;const auto name=std::string(resize?"resize-update-":"static-")+std::to_string(mode);const auto path=output/(name+".ppm");renderer.delegate.SetCaptureOutput(path,4);
            Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.delegate.CaptureSucceeded(),"actual sorted/cache benchmark scene");std::vector<std::uint8_t> image;Check(asset_io::Read(path,image),"queue image");
            if(!mode)expected=image;else Check(image==expected,"all four independent sorting/cache paths preserve full GPU image");
            const auto binds=renderer.last.opaqueBindings;Check(renderer.draws.draws==192&&binds.pipelines==1&&binds.rootSignatures==1&&binds.constants==192,"PSO already once; per-draw constants always update");
            if(!cache)Check(binds.descriptorTables==192&&binds.vertexBuffers==192&&binds.indexBuffers==192,"uncached path keeps actual Set calls");
            if(!mode){baselineTables=binds.descriptorTables;baselineVertices=binds.vertexBuffers;}
            if(cache)Check(binds.descriptorTables<baselineTables&&binds.vertexBuffers<baselineVertices&&binds.indexBuffers==binds.vertexBuffers,"binding cache reduces real table/VB/IB calls");
            if(cache&&sorting)Check(binds.descriptorTables==4&&binds.vertexBuffers<=48,"sorted textures and 24-material/two-mesh groups");
            std::cout<<name<<": tables="<<binds.descriptorTables<<", VB/IB="<<binds.vertexBuffers<<", constants="<<binds.constants<<"\n";
        }
    }
}
void G00Contracts()
{
    Check(RenderPassCount==16,"G01 freezes sixteen render pass slots");
    for(std::size_t index=0;index<RenderPassCount;++index)
        for(std::size_t other=index+1;other<RenderPassCount;++other)
            Check(std::string_view(RenderPassName(static_cast<RenderPassId>(index)))!=RenderPassName(static_cast<RenderPassId>(other)),"render pass names unique");
    RenderExperiment experiment=RenderExperiment::G07;
    Check(ParseRenderExperiment("off",experiment)&&experiment==RenderExperiment::Off,"experiment off parses");
    Check(ParseRenderExperiment("g01",experiment)&&experiment==RenderExperiment::G01,"experiment g01 parses");
    Check(ParseRenderExperiment("g15",experiment)&&experiment==RenderExperiment::G15,"experiment g15 parses");
    Check(!ParseRenderExperiment("g00",experiment)&&!ParseRenderExperiment("g16",experiment)&&!ParseRenderExperiment("G01",experiment)&&!ParseRenderExperiment("g1",experiment)&&!ParseRenderExperiment("",experiment),"unknown experiment rejected");
    Check(rhi::VertexLayoutVersionLegacy!=rhi::VertexLayoutVersionCompressed,"compressed vertex layout has its own version");
    Check(rhi::GSeriesLayoutVersions[0].version==rhi::VertexCompressedLayoutVersion,"layout registry starts with G03");
    for(const auto& record:rhi::GSeriesLayoutVersions)
        Check(record.version>=20&&record.system&&record.fields,"G-series layout version reserved above existing layouts");
    RenderStats stats;stats.gpuPassMilliseconds[0]=1.5f;stats.gpuPassMilliseconds[5]=2.5f;
    Check(stats.GpuTotalMilliseconds()==4.0f,"stats total sums fixed slots");
}
void Telemetry(const std::filesystem::path& output)
{
    se::RenderTelemetry telemetry(10.0f);
    rhi::Statistics before;before.stateTransitions=5;before.pipelineCreations=2;before.uploadedBytes=100;before.memoryUsedBytes=64ull*1024*1024;before.persistentDescriptors=7;
    telemetry.BeginFrame(9,before);
    auto after=before;after.submittedFrames=1;after.stateTransitions=8;after.pipelineCreations=3;after.uploadedBytes=150;
    telemetry.EndFrame(after,42,7,300,1.5f);
    const auto* latest=telemetry.Latest();
    Check(latest&&latest->drawCalls==42&&latest->triangles==300&&latest->stateChanges==3&&latest->psoCreated==1&&latest->uploadBytes==50&&latest->vramUsedMiB==64&&latest->cpuPrepareMilliseconds==1.5f,"telemetry counters");
    rhi::GpuTiming timing;timing.submission=1;timing.frequency=1000;timing.forward=true;
    timing.passTicks={0,1,2,3,4,19000,19100,20000};timing.ticks={0,19000,19100,20000};timing.frameMilliseconds=20;
    const std::array<rhi::GpuTiming,1> timings{timing};
    telemetry.ObserveTimings(timings);
    latest=telemetry.Latest();
    Check(latest&&latest->GpuTotalMilliseconds()>18.0f&&latest->longFrameReason&&telemetry.LongFrames()==1,"telemetry late timestamps and long frame");
    Check(std::string_view(latest->longFrameReason)=="Opaque","long frame attributes opaque pass");
    std::string error;const auto csv=output/"telemetry.csv";
    Check(telemetry.ExportCsv(csv,error),error.empty()?"telemetry csv export":error.c_str());
    Check(std::filesystem::exists(csv),"telemetry csv exists");
}
void Variants()
{
    const auto mask=se::MaterialFeatureMask(se::MaterialFeature::AlbedoMap)|se::MaterialFeatureMask(se::MaterialFeature::NormalMap);
    const auto macros=se::VariantMacros(mask);
    Check(macros.size()==2&&macros[0].name=="SE_FEATURE_ALBEDO_MAP"&&macros[0].value=="1"&&macros[1].name=="SE_FEATURE_NORMAL_MAP","variant macros follow the feature table");
    Check(se::VariantMacros(0).empty(),"no feature bits produce no macros");
    Check(se::VariantId(0x10)!=se::VariantId(0x01)&&se::VariantId(0).size()==8,"variant id is stable and distinct");
}
void MaterialInstances()
{
    auto parent=std::make_shared<se::PbrRenderMaterial>();
    parent->metallic=0.2f;parent->roughness=0.4f;parent->featureMask=se::FeatureMaskFor(*parent);
    se::MaterialInstance instance;instance.parent=parent;instance.metallic=0.9f;instance.roughness=0.6f;
    instance.baseColor={1,0,0,1};instance.addFeatures=se::MaterialFeatureMask(se::MaterialFeature::DoubleSided);
    const auto resolved=se::ResolveMaterialInstance(instance);
    Check(resolved&&resolved->metallic==0.9f&&resolved->roughness==0.6f&&resolved->color.x==1.0f
        &&(resolved->featureMask&se::MaterialFeatureMask(se::MaterialFeature::DoubleSided)),"material instance overrides");
    Check(parent->metallic==0.2f,"material instance does not mutate parent");
    se::VariantInventory inventory;
    Check(inventory.Add(resolved->featureMask)&&!inventory.Add(resolved->featureMask)
        &&inventory.Distinct()==1&&inventory.Total()==2,"variant inventory counts");
}
void VertexCompression()
{
    std::vector<se::MeshVertex> vertices(3);
    vertices[0].position={0,0,0};vertices[1].position={1,0,0};vertices[2].position={0,2,0};
    vertices[0].normal={0,0,1};vertices[1].normal={1,0,0};vertices[2].normal={0,1,0};
    vertices[0].uv={0.25f,0.75f};vertices[1].uv={0.5f,0.5f};vertices[2].uv={0,1};
    se::Aabb bounds;bounds.minimum={0,0,0};bounds.maximum={1,2,0};
    std::vector<se::PackedVertex> packed;se::MeshQuantization quantization;std::string error;
    Check(se::CompressMeshVertices(vertices,bounds,packed,quantization,error)&&packed.size()==3
        &&sizeof(se::PackedVertex)==16,"compress vertices to sixteen bytes");
    const auto position=se::DecodePackedPosition(packed[2],quantization);
    Check(std::fabs(position.x)<1e-4f&&std::fabs(position.y-2.0f)<1e-4f,"quantized position round trip");
    const auto normal=se::DecodePackedNormal(packed[1]);
    Check(normal.x>0.99f,"octahedral normal round trip");
    float u=0,v=0;se::DecodePackedUv(packed[0],u,v);
    Check(std::fabs(u-0.25f)<1e-3f&&std::fabs(v-0.75f)<1e-3f,"half uv round trip");
}
void MeshCache()
{
    const std::uint32_t indices[]{0,1,2,2,1,3};
    const float ratio=se::AverageCacheMissRatio(indices);
    Check(std::fabs(ratio-4.0f/6.0f)<1e-6f,"ACMR counts unique vertices in FIFO cache");
    Check(se::AverageCacheMissRatio(std::span<const std::uint32_t>{})==0.0f,"empty index stream has no cache misses");
    std::vector<std::uint32_t> grid;
    const int side=5;
    for(int y=0;y<side-1;++y)for(int x=0;x<side-1;++x)
    {
        const std::uint32_t a=static_cast<std::uint32_t>(y*side+x),b=a+1,c=a+static_cast<std::uint32_t>(side),d=c+1;
        for(const std::uint32_t value:{a,b,c,b,d,c})grid.push_back(value);
    }
    const float before=se::AverageCacheMissRatio(grid);
    const auto optimized=se::OptimizeVertexCache(grid);
    const float after=se::AverageCacheMissRatio(optimized);
    Check(optimized.size()==grid.size()&&after<=before+1e-6f,"vertex cache reorder does not increase ACMR");
}
void TexturePipeline(const std::filesystem::path& output)
{
    std::vector<std::uint8_t> pixels(8*8*4,255);
    for(std::size_t index=0;index<pixels.size();index+=4){pixels[index]=220;pixels[index+1]=40;pixels[index+2]=30;}
    se::Bc1Image compressed;std::string error;
    Check(se::CompressBc1(pixels,8,8,compressed,error),"BC1 compress");
    Check(compressed.width==8&&compressed.height==8&&compressed.blocks.size()==2*2*8,"BC1 block layout");
    std::vector<std::uint8_t> decoded;
    se::DecodeBc1(compressed,decoded);
    Check(decoded.size()==pixels.size()&&decoded[0]>200&&decoded[1]<80&&decoded[2]<80,"BC1 decode keeps base color");

    se::TextureResidency residency(100);
    Check(residency.Request(1,3,0,64,1),"residency request one");
    Check(residency.Request(2,3,0,64,2),"residency request two");
    Check(residency.LoadedBytes()<=100,"residency trims to budget");
    const auto* first=residency.Find(1);
    Check(first&&first->desiredMip==2,"oldest texture drops finest mips first");

    // G04.04：BC1 派生纹理烘焙、缓存键与 .sebc 往返。
    se::ImageData image{8,8};
    image.rgba8.resize(8*8*4,255);
    for(std::size_t index=0;index<image.rgba8.size();index+=4){image.rgba8[index]=220;image.rgba8[index+1]=40;image.rgba8[index+2]=30;}
    const auto sourceKey=se::ImageContentKey(image);
    Check(!sourceKey.empty(),"bake source content key");
    se::BakedTexture linear;
    Check(se::BakeBc1Texture(image,se::BakedTextureFormat::Bc1Unorm,linear,error),"BC1 bake");
    Check(linear.width==8&&linear.height==8&&linear.mipLevels==4&&linear.mipOffsets.size()==4,"BC1 bake full mip chain");
    Check(linear.bytes.size()==56&&linear.mipOffsets[0]==0&&linear.mipOffsets[1]==32
        &&linear.mipOffsets[2]==40&&linear.mipOffsets[3]==48,"BC1 mip byte offsets");
    Check(se::Bc1MipLevels(8,8)==4&&se::Bc1MipBytes(8,8,0)==32&&se::Bc1MipBytes(8,8,3)==8,"BC1 mip size formula");
    se::BakedTexture srgb;
    Check(se::BakeBc1Texture(image,se::BakedTextureFormat::Bc1UnormSrgb,srgb,error)&&srgb.bytes==linear.bytes,"sRGB bake shares block data");
    const auto linearKey=se::BakedTextureKey(sourceKey,se::BakedTextureFormat::Bc1Unorm);
    const auto srgbKey=se::BakedTextureKey(sourceKey,se::BakedTextureFormat::Bc1UnormSrgb);
    Check(linearKey!=srgbKey&&linearKey.find(se::TextureBakeEncoderVersion)!=std::string::npos,"bake key separates format and encoder version");
    se::ImageData broken{4,4};broken.rgba8.assign(4,0);se::BakedTexture invalid;std::string bakeError;
    Check(!se::BakeBc1Texture(broken,se::BakedTextureFormat::Bc1Unorm,invalid,bakeError)
        &&bakeError=="Rendering.TextureEncodeFailed"&&invalid.bytes.empty(),"mismatched source bytes fail with stable key");

    const auto bakeDirectory=output/"bc1-cache";const auto path=bakeDirectory/"linear.sebc";
    std::filesystem::remove_all(bakeDirectory); // 磁盘缓存跨运行保留，测试先清空自己的目录。
    Check(se::WriteBakedTexture(path,linear),"write .sebc");
    se::BakedTexture loaded;
    Check(se::ReadBakedTexture(path,loaded)&&loaded.width==8&&loaded.height==8&&loaded.mipLevels==4
        &&loaded.bytes==linear.bytes&&loaded.mipOffsets==linear.mipOffsets,"read .sebc round trip");
    std::vector<std::uint8_t> fileBytes;
    Check(se::asset_io::Read(path,fileBytes)&&fileBytes.size()>8,"read .sebc bytes");
    fileBytes.resize(fileBytes.size()-1);
    {std::ofstream out(path,std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char*>(fileBytes.data()),static_cast<std::streamsize>(fileBytes.size()));}
    Check(!se::ReadBakedTexture(path,loaded),"truncated .sebc rejected");

    se::TextureBakeCache cache;cache.SetDirectory(bakeDirectory/"store");
    const auto* memory=cache.Bake(image,sourceKey,se::BakedTextureFormat::Bc1Unorm,bakeError);
    Check(memory&&cache.Count()==1&&cache.Bytes()==56&&cache.DiskStores()==1,"bake cache stores first texture");
    Check(cache.Bake(image,sourceKey,se::BakedTextureFormat::Bc1Unorm,bakeError)==memory
        &&cache.Count()==1&&cache.DiskStores()==1,"bake cache reuses memory entry");
    se::TextureBakeCache reopened;reopened.SetDirectory(bakeDirectory/"store");
    const auto* fromDisk=reopened.Bake(image,sourceKey,se::BakedTextureFormat::Bc1Unorm,bakeError);
    Check(fromDisk&&fromDisk->bytes==memory->bytes&&reopened.DiskLoads()==1,"bake cache loads .sebc across instances");

    // G04.05：BC5 双通道块用于法线贴图，mip 链与 .sebc 格式按 16 字节块。
    se::ImageData normalSource{8,8};
    normalSource.rgba8.resize(8*8*4,255);
    for(std::uint32_t y=0;y<8;++y)for(std::uint32_t x=0;x<8;++x)
    {
        auto* pixel=normalSource.rgba8.data()+(std::size_t(y)*8+x)*4;
        pixel[0]=static_cast<std::uint8_t>(32+x*24);pixel[1]=static_cast<std::uint8_t>(32+y*24);
        pixel[2]=180;pixel[3]=255;
    }
    se::Bc5Image bc5;std::string bc5Error;
    Check(se::CompressBc5(normalSource.rgba8,8,8,bc5,bc5Error),"BC5 compress");
    Check(bc5.width==8&&bc5.height==8&&bc5.blocks.size()==2*2*16,"BC5 block layout");
    std::vector<std::uint8_t> bc5Decoded;se::DecodeBc5(bc5,bc5Decoded);
    int bc5MaximumError=0;
    for(std::size_t index=0;index<bc5Decoded.size();index+=4)
        bc5MaximumError=(std::max)(bc5MaximumError,(std::max)(
            std::abs(int(bc5Decoded[index])-int(normalSource.rgba8[index])),
            std::abs(int(bc5Decoded[index+1])-int(normalSource.rgba8[index+1]))));
    Check(bc5MaximumError<=16,"BC5 channel round trip error");
    Check(bc5Decoded[2]==0&&bc5Decoded[3]==255,"BC5 decode keeps blue zero and alpha one");
    std::vector<se::Bc5Image> bc5Chain;
    Check(se::CompressBc5MipChain(normalSource.rgba8,8,8,4,bc5Chain,bc5Error)&&bc5Chain.size()==4,"BC5 mip chain");
    se::BakedTexture baked5;
    Check(se::BakeBc5Texture(normalSource,baked5,bc5Error)&&baked5.mipLevels==4
        &&baked5.format==se::BakedTextureFormat::Bc5Unorm,"BC5 bake");
    Check(se::BakedMipBytes(se::BakedTextureFormat::Bc5Unorm,8,8,0)==64
        &&baked5.bytes.size()==112&&baked5.mipOffsets[1]==64&&baked5.mipOffsets[2]==80
        &&baked5.mipOffsets[3]==96,"BC5 bake mip layout");
    const auto bc5Key=se::BakedTextureKey(sourceKey,se::BakedTextureFormat::Bc5Unorm);
    Check(bc5Key!=linearKey&&bc5Key.rfind("bc5:",0)==0,"BC5 key is distinct");
    const auto bc5Path=bakeDirectory/"normal.sebc";
    Check(se::WriteBakedTexture(bc5Path,baked5)&&se::ReadBakedTexture(bc5Path,loaded)
        &&loaded.format==se::BakedTextureFormat::Bc5Unorm&&loaded.bytes==baked5.bytes,"BC5 .sebc round trip");

    // G04.06：驻留按压缩块最小字节记账，预算不足时降级最久未用。
    se::TextureResidency blockResidency(8192);
    Check(blockResidency.Request(7,7,0,4096,1,16)&&blockResidency.LoadedBytes()==5488,"BC5 residency counts the full chain");
    blockResidency.SetBudget(2048);
    Check(blockResidency.TrimToBudget()==1,"trim degrades one texture");
    const auto* block=blockResidency.Find(7);
    Check(block&&block->desiredMip==1&&block->loadedBytes==1392,"trim drops the finest mip first");

    // G04.08：BC4 单通道（AO/粗糙度）块与 mip 链。
    se::ImageData aoSource{8,8};
    aoSource.rgba8.resize(8*8*4,255);
    for(std::uint32_t y=0;y<8;++y)for(std::uint32_t x=0;x<8;++x)
        aoSource.rgba8[(std::size_t(y)*8+x)*4]=static_cast<std::uint8_t>(20+x*28);
    se::Bc4Image bc4;std::string bc4Error;
    Check(se::CompressBc4(aoSource.rgba8,8,8,bc4,bc4Error),"BC4 compress");
    Check(bc4.blocks.size()==2*2*8,"BC4 block layout");
    std::vector<std::uint8_t> bc4Decoded;se::DecodeBc4(bc4,bc4Decoded);
    int bc4ErrorMax=0;
    for(std::size_t index=0;index<bc4Decoded.size();index+=4)
        bc4ErrorMax=(std::max)(bc4ErrorMax,std::abs(int(bc4Decoded[index])-int(aoSource.rgba8[index])));
    Check(bc4ErrorMax<=16&&bc4Decoded[1]==0&&bc4Decoded[3]==255,"BC4 single channel round trip");
    se::BakedTexture baked4;
    Check(se::BakeBc4Texture(aoSource,baked4,bc4Error)&&baked4.mipLevels==4
        &&baked4.format==se::BakedTextureFormat::Bc4Unorm,"BC4 bake");
    Check(se::BakedMipBytes(se::BakedTextureFormat::Bc4Unorm,8,8,0)==32
        &&baked4.bytes.size()==56&&baked4.mipOffsets[1]==32,"BC4 bake mip layout");
    const auto bc4Path=bakeDirectory/"ao.sebc";
    Check(se::WriteBakedTexture(bc4Path,baked4)&&se::ReadBakedTexture(bc4Path,loaded)
        &&loaded.format==se::BakedTextureFormat::Bc4Unorm&&loaded.bytes==baked4.bytes,"BC4 .sebc round trip");

    // G04-AC04：128 MiB 预算下 60 张 2048² 纹理降级不失败。
    se::TextureResidency largeResidency(128ull*1024*1024);
    for(std::uint64_t id=1;id<=60;++id)
        Check(largeResidency.Request(id,12,0,2097152,id,8),"2048 residency request");
    Check(largeResidency.Entries().size()==60,"128 MiB budget keeps every texture entry");
    Check(largeResidency.LoadedBytes()<=128ull*1024*1024,"128 MiB budget stays within limit");
    bool anyDegraded=false;
    for(const auto& [id,entry]:largeResidency.Entries())anyDegraded=anyDegraded||entry.desiredMip>0;
    Check(anyDegraded,"128 MiB budget degrades the oldest 2048 textures");
}
void LodChains(const std::filesystem::path& output)
{
    se::StaticModel model;auto mesh=se::MakeUnitSphere();mesh.name="Sphere";
    const auto originalTriangles=mesh.indices.size()/3;
    model.meshes.push_back(mesh);
    se::ModelNode node;node.name="Sphere";node.meshes={0};model.nodes.push_back(node);
    std::vector<se::AssetDiagnostic> diagnostics;se::LodChain chain;
    Check(se::BakeLodChain(model,0,4,chain,diagnostics),"LOD bake");
    Check(chain.levels.size()==4&&chain.levels[0].indices.size()==mesh.indices.size(),"LOD level zero is the original mesh");
    for(std::size_t level=1;level<chain.levels.size();++level)
    {
        Check(chain.levels[level].indices.size()<chain.levels[level-1].indices.size(),"LOD levels reduce triangles");
        Check(chain.levels[level].indices.size()%3==0,"LOD indices stay triangles");
        Check(chain.levels[level].worldError>=chain.levels[level-1].worldError,"LOD error grows with coarseness");
    }
    Check(chain.levels[3].indices.size()/3*2<originalTriangles,"coarsest LOD is below half the triangles");
    const auto path=output/"sphere.selod";std::string error;
    Check(se::WriteLodChain(path,chain,error),"write .selod");
    se::LodChain loaded;
    Check(se::ReadLodChain(path,loaded,error)&&loaded.levels.size()==4
        &&loaded.levels[2].indices==chain.levels[2].indices&&loaded.vertexCount==chain.vertexCount,"read .selod round trip");
    std::vector<std::uint8_t> bytes;Check(se::asset_io::Read(path,bytes)&&bytes.size()>8,"read .selod bytes");
    bytes.resize(bytes.size()-1);
    {std::ofstream out(path,std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    Check(!se::ReadLodChain(path,loaded,error),"truncated .selod rejected");

    const se::Vec3 center{0,0,0};se::LodView view;view.viewportHeight=1080;view.verticalFov=se::Pi/3;
    std::cout<<"LOD levels:";
    for(const auto& level:chain.levels)std::cout<<" "<<level.indices.size()/3<<"tris/err="<<level.worldError;
    std::cout<<" radius="<<chain.radius<<"\n";
    view.position={0,0,1};const auto nearLevel=se::SelectLodLevel(chain,center,view,2.0f,0);
    view.position={0,0,300};const auto farLevel=se::SelectLodLevel(chain,center,view,2.0f,0);
    Check(nearLevel==0&&farLevel>0,"close camera stays at LOD0 while a far camera coarsens");
    int switches=0;auto level=farLevel;
    for(int index=0;index<20;++index)
    {
        const auto next=se::SelectLodLevel(chain,center,view,2.0f,level);
        switches+=next!=level;level=next;
    }
    Check(switches==0,"static camera does not oscillate between LODs");
    // 朝相机推进：级别必须单调变细，不允许在边界抖动。
    int regressions=0;auto previous=level;
    for(float distance=300;distance>=2;distance-=0.5f)
    {
        view.position={0,0,distance};
        const auto next=se::SelectLodLevel(chain,center,view,2.0f,previous);
        regressions+=next>previous;previous=next;
    }
    Check(regressions==0&&previous==0,"approaching camera monotonically reaches LOD0");
    // G05.02：dither 过渡 0.15 s 内完成、blend 单调、完成后保持稳定。
    se::LodTransition transition;
    view.position={0,0,1};se::UpdateLodTransition(chain,center,view,2.0f,0.016f,transition);
    Check(transition.current==0&&!transition.active,"near start stays at LOD0 without a transition");
    view.position={0,0,300};
    const auto switched=se::UpdateLodTransition(chain,center,view,2.0f,0.016f,transition);
    Check(transition.active&&se::LodBlendWeight(transition)==0.0f,"level change starts at zero blend");
    float previousBlend=0;int steps=0;
    for(;steps<40&&transition.active;++steps)
    {
        const auto now=se::UpdateLodTransition(chain,center,view,2.0f,0.016f,transition);
        const auto blend=se::LodBlendWeight(transition);
        Check(now==switched&&blend>=previousBlend,"transition keeps the target level and grows monotonically");
        previousBlend=blend;
    }
    Check(!transition.active&&previousBlend==1.0f,"transition completes after the 0.15 s window");
    const auto stable=se::UpdateLodTransition(chain,center,view,2.0f,0.016f,transition);
    Check(stable==switched&&!transition.active&&se::LodBlendWeight(transition)==1.0f,"completed transition stays stable");
    // G05.03：4×4 Bayer dither 在 0/0.5/1 三个 blend 下的像素分配。
    Check(se::Bayer4x4Threshold(0,0)<se::Bayer4x4Threshold(1,0),"Bayer matrix is ordered");
    int previousPixels=0;
    for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<4;++x)previousPixels+=se::LodDitherUsesPrevious(0.0f,x,y);
    Check(previousPixels==16,"zero blend keeps every pixel on the previous LOD");
    previousPixels=0;
    for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<4;++x)previousPixels+=se::LodDitherUsesPrevious(1.0f,x,y);
    Check(previousPixels==0,"full blend uses only the new LOD");
    previousPixels=0;
    for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<4;++x)previousPixels+=se::LodDitherUsesPrevious(0.5f,x,y);
    Check(previousPixels==8,"half blend mixes both LODs by Bayer threshold");
    // G05.05：世界变换后的模型选择。
    view.position={0,0,1};
    se::LodTransition worldTransition;
    const auto worldNear=se::UpdateModelLod(chain,{0,0,0},se::Translation({0,0,0}),view,2.0f,0.0f,worldTransition);
    Check(worldNear==0&&!worldTransition.active,"identity world keeps LOD0 at close range");
    const auto worldFar=se::UpdateModelLod(chain,{0,0,0},se::Translation({0,0,299}),view,2.0f,0.016f,worldTransition);
    Check(worldFar>0&&worldTransition.active,"world translation coarsens the LOD through a transition");
}
void HlodCells(const std::filesystem::path& output)
{
    // G05.09：64 m 单元划分、材质分组、代理覆盖与 .sehlod 往返。
    auto sphere=se::MakeUnitSphere();const auto sourceTriangles=sphere.indices.size()/3;
    const auto sourceVertices=sphere.vertices.size();
    std::vector<se::HlodInstance> instances;
    const auto add=[&](se::Vec3 position,float error,std::string material)
    {
        instances.push_back({&sphere,nullptr,se::Translation(position),error,std::move(material)});
    };
    add({5,0,0},0.2f,"material-a");add({20,0,0},0.3f,"material-a");add({25,0,0},0.1f,"material-b");
    add({70,0,0},0.25f,"material-a");add({64,0,0},0.15f,"material-a");add({5,0,70},0.05f,"material-a");
    se::HlodScene scene;std::vector<se::AssetDiagnostic> diagnostics;
    Check(se::BakeHlod(instances,se::HlodCellSize,scene,diagnostics),"HLOD bake");
    Check(scene.instanceCount==6&&scene.cells.size()==3,"HLOD splits six instances into three cells");
    const auto& firstCell=scene.cells[0];
    Check(firstCell.coordinate.x==0&&firstCell.coordinate.z==0&&firstCell.parts.size()==2,"HLOD groups by material inside a cell");
    const auto& secondCell=scene.cells[1];
    Check(secondCell.coordinate.x==0&&secondCell.coordinate.z==1&&secondCell.parts.size()==1,"HLOD assigns the z boundary cell");
    const auto& thirdCell=scene.cells[2];
    Check(thirdCell.coordinate.x==1&&thirdCell.coordinate.z==0&&thirdCell.parts.size()==1,"HLOD assigns x=64 m to the positive cell only");
    Check(thirdCell.parts[0].mesh.indices.size()/3==sourceTriangles*2,"HLOD merges two boundary instances in one proxy");
    Check(std::abs(firstCell.worldError-0.3f)<1e-6f&&std::abs(secondCell.worldError-0.05f)<1e-6f
        &&std::abs(thirdCell.worldError-0.25f)<1e-6f,"HLOD cell error is the instance maximum");
    std::uint64_t proxyTriangles=0,proxyVertices=0;
    for(const auto& cell:scene.cells)
    {
        Check(cell.radius>0&&std::isfinite(cell.radius)&&std::isfinite(cell.center.x),"HLOD cell bounds are finite");
        for(const auto& part:cell.parts){proxyTriangles+=part.mesh.indices.size()/3;proxyVertices+=part.mesh.vertices.size();}
    }
    Check(proxyTriangles==std::uint64_t(sourceTriangles)*instances.size(),"every instance appears in exactly one proxy");
    Check(proxyVertices==std::uint64_t(sourceVertices)*instances.size(),"proxy vertices are not duplicated across cells");
    const auto path=output/"cells.sehlod";std::string error;
    Check(se::WriteHlodScene(path,scene,error),"write .sehlod");
    se::HlodScene loaded;
    Check(se::ReadHlodScene(path,loaded,error)&&loaded.cells.size()==scene.cells.size()
        &&loaded.instanceCount==scene.instanceCount&&loaded.cells[2].parts[0].mesh.indices==scene.cells[2].parts[0].mesh.indices,
        "read .sehlod round trip");
    std::vector<std::uint8_t> bytes;Check(se::asset_io::Read(path,bytes)&&bytes.size()>8,"read .sehlod bytes");
    bytes.resize(bytes.size()-1);
    {std::ofstream out(path,std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    Check(!se::ReadHlodScene(path,loaded,error),"truncated .sehlod rejected");
    // 单元选择：远距离启用代理，1.25× 迟滞在阈值附近保持稳定。
    const auto& cell=scene.cells[0];se::HlodView view;view.viewportHeight=1080;view.verticalFov=se::Pi/3;
    const auto distanceFor=[&](float projected){return cell.worldError*view.viewportHeight*0.5f
        /(std::tan(view.verticalFov*0.5f)*projected);};
    view.position=cell.center+se::Vec3{0,0,distanceFor(50)};
    Check(!se::SelectHlodProxy(cell,view,2.0f,false),"near camera keeps individual instances");
    view.position=cell.center+se::Vec3{0,0,distanceFor(0.5f)};
    Check(se::SelectHlodProxy(cell,view,2.0f,false),"far camera switches to the cell proxy");
    view.position=cell.center+se::Vec3{0,0,distanceFor(2.4f)};
    Check(se::SelectHlodProxy(cell,view,2.0f,true),"proxy hysteresis keeps an already proxied cell");
    Check(!se::SelectHlodProxy(cell,view,2.0f,false),"missing hysteresis margin does not switch on");
    view.position=cell.center+se::Vec3{0,0,distanceFor(1.5f)};
    Check(se::SelectHlodProxy(cell,view,2.0f,false),"error inside threshold/1.25 switches in");
    Check(se::SelectHlodProxy(cell,view,2.0f,true),"error below threshold*1.25 stays proxied");
    std::cout<<"HLOD cells: "<<scene.cells.size()<<", proxy triangles="<<proxyTriangles
        <<", source triangles="<<sourceTriangles*instances.size()<<", cell error="<<cell.worldError<<"\n";
}
void HlodStreaming(const std::filesystem::path& output)
{
    // G05.11：`.sehlod` 加载与按相机半径驻留；非驻留单元由调用方回退实例。
    auto sphere=se::MakeUnitSphere();
    std::vector<se::HlodInstance> instances;
    instances.push_back({&sphere,nullptr,se::Translation({5,0,0}),0.2f,"a"});
    instances.push_back({&sphere,nullptr,se::Translation({70,0,0}),0.2f,"a"});
    se::HlodScene scene;std::vector<se::AssetDiagnostic> diagnostics;
    Check(se::BakeHlod(instances,se::HlodCellSize,scene,diagnostics),"HLOD stream bake");
    const auto path=output/"stream.sehlod";std::string error;
    Check(se::WriteHlodScene(path,scene,error),"HLOD stream write");
    se::HlodStream stream;
    Check(stream.Load(path,error)&&stream.Loaded()&&stream.Scene().cells.size()==2,"HLOD stream load");
    Check(stream.UpdateResidency({5,0,0},10.0f)==1&&stream.Resident(0)&&!stream.Resident(1),
        "HLOD stream keeps only the nearby cell around the first cell");
    Check(stream.UpdateResidency({70,0,0},10.0f)==1&&!stream.Resident(0)&&stream.Resident(1),
        "HLOD stream moves residency to the second cell");
    Check(stream.UpdateResidency({0,0,0},0.0f)==2&&stream.ResidentCount()==2,"HLOD stream zero radius keeps every cell");
    se::HlodStream missing;
    Check(!missing.Load(output/"missing.sehlod",error)&&!missing.Loaded(),"HLOD stream missing file rejected");
    stream.Clear();
    Check(!stream.Loaded()&&stream.ResidentCount()==0,"HLOD stream clear resets residency");
    std::cout<<"HLOD stream: cells="<<scene.cells.size()<<", residency switches between cells\n";
}
void HlodStreamAsync(const std::filesystem::path& output)
{
    // G05.13：后台读取 `.sehlod`，轮询完成且只交付一次；缺失文件以 Failed 结束。
    const auto path=output/"stream.sehlod";
    se::HlodStreamer streamer;std::string error;
    Check(streamer.Start(path,error)&&streamer.Running(),"HLOD async start");
    se::HlodScene scene;
    auto state=se::HlodStreamer::State::Pending;
    for(unsigned attempt=0;attempt<2000&&state==se::HlodStreamer::State::Pending;++attempt)
    {
        state=streamer.Poll(scene,error);
        if(state==se::HlodStreamer::State::Pending)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(state==se::HlodStreamer::State::Ready&&scene.cells.size()==2,"HLOD async ready with both cells");
    Check(streamer.Poll(scene,error)==se::HlodStreamer::State::Idle,"HLOD async result delivered once");
    Check(streamer.Start(output/"missing.sehlod",error),"HLOD async missing start");
    state=se::HlodStreamer::State::Pending;
    for(unsigned attempt=0;attempt<2000&&state==se::HlodStreamer::State::Pending;++attempt)
    {
        state=streamer.Poll(scene,error);
        if(state==se::HlodStreamer::State::Pending)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(state==se::HlodStreamer::State::Failed&&!error.empty(),"HLOD async missing file fails");
    streamer.Cancel();
    Check(!streamer.Running(),"HLOD async cancel is idempotent");
    std::cout<<"HLOD async: background read, single delivery, missing-file failure and cancel passed\n";
}
void HlodProxyBudget()
{
    // G05.13：代理显存预算、每帧上传上限与最久未用淘汰；选中/本帧使用的单元不被淘汰。
    se::HlodProxyCache cache;cache.Configure(1000,2);
    Check(cache.Fits(1000)&&cache.Fits(600),"proxy cache accepts proxies within budget");
    cache.BeginFrame();
    Check(cache.CanUpload(),"proxy cache upload budget starts open");
    cache.AddUpload(100);cache.AddUpload(100);
    Check(!cache.CanUpload()&&cache.UploadsThisFrame()==2,"proxy cache enforces per-frame uploads");
    cache.Insert(0,400);cache.Insert(1,400);cache.Insert(2,400);
    Check(cache.Bytes()==1200&&cache.OverBudget(),"proxy cache tracks over-budget bytes");
    Check(!cache.Fits(400),"proxy cache rejects an upload that would exceed the budget");
    cache.BeginFrame();cache.Touch(0);cache.Touch(1);
    std::uint32_t evicted=99;
    Check(cache.SelectEviction(evicted)&&evicted==2,"proxy cache evicts the oldest untouched cell");
    cache.Erase(evicted);
    Check(cache.Bytes()==800&&!cache.OverBudget(),"proxy cache returns evicted bytes");
    cache.BeginFrame();cache.Touch(0);cache.Touch(1);cache.Touch(2);
    evicted=99;Check(!cache.SelectEviction(evicted),"proxy cache keeps cells used this frame");
    Check(cache.Stats().uploads==2&&cache.Stats().uploadedBytes==200&&cache.Stats().peakBytes==1200,
        "proxy cache upload/peak statistics");
    cache.Reset();
    Check(cache.Bytes()==0&&cache.Stats().frames==0,"proxy cache reset clears bytes and statistics");
    std::cout<<"HLOD proxy cache: budget, per-frame uploads, LRU eviction and reset passed\n";
}
void Meshlets(const std::filesystem::path& output)
{
    // G07 原型：64 顶点/124 三角形 cluster 烘焙、覆盖/包围球/法线锥、`.semsl` 往返与保守剔除。
    auto mesh=se::MakeUnitSphere();
    for(auto& vertex:mesh.vertices)vertex.position[2]-=20.0f;
    se::MeshletSet set;std::vector<se::AssetDiagnostic> diagnostics;
    Check(se::BakeMeshlets(mesh,set,diagnostics),"meshlet bake");
    Check(set.sourceVertexCount==mesh.vertices.size()&&set.triangleCount==mesh.indices.size()/3,"meshlet source counts");
    std::map<std::array<std::uint32_t,3>,std::uint32_t> coverage;
    std::uint64_t triangles=0,coneValid=0;
    for(const auto& cluster:set.clusters)
    {
        Check(cluster.vertices.size()<=se::MeshletMaxVertices&&cluster.indices.size()%3==0
            &&cluster.indices.size()/3<=se::MeshletMaxTriangles,"meshlet cluster limits");
        Check(std::isfinite(cluster.radius)&&cluster.radius>=0,"meshlet cluster radius");
        for(const auto local:cluster.indices)Check(local<cluster.vertices.size(),"meshlet local index");
        for(const auto source:cluster.vertices)Check(source<mesh.vertices.size(),"meshlet source vertex");
        for(std::size_t index=0;index<cluster.indices.size();index+=3)
        {
            std::array<std::uint32_t,3> triangle{cluster.vertices[cluster.indices[index]],
                cluster.vertices[cluster.indices[index+1]],cluster.vertices[cluster.indices[index+2]]};
            std::sort(triangle.begin(),triangle.end());++coverage[triangle];++triangles;
        }
        for(const auto source:cluster.vertices)
        {
            const auto& position=mesh.vertices[source].position;
            const auto distance=se::Length(se::Vec3{position[0],position[1],position[2]}-cluster.center);
            Check(distance<=cluster.radius+1e-3f,"meshlet bound contains every vertex");
        }
        if(cluster.coneValid)++coneValid;
    }
    Check(triangles==set.triangleCount&&coverage.size()==set.triangleCount,"meshlet covers every source triangle once");
    for(const auto& [triangle,count]:coverage)Check(count==1,"meshlet triangle appears exactly once");
    const auto path=output/"sphere.semsl";std::string error;
    Check(se::WriteMeshletSet(path,set,error),"meshlet write");
    se::MeshletSet loaded;
    Check(se::ReadMeshletSet(path,loaded,error)&&loaded.clusters.size()==set.clusters.size()
        &&loaded.triangleCount==set.triangleCount,"meshlet round trip");
    std::vector<std::uint8_t> bytes;
    Check(se::asset_io::Read(path,bytes)&&bytes.size()>8,"meshlet bytes readable");
    bytes.resize(bytes.size()/2);
    Check(se::asset_io::WriteAtomic(output/"truncated.semsl",bytes,true)&&!se::ReadMeshletSet(output/"truncated.semsl",loaded,error),
        "truncated meshlet file rejected");
    const auto clip=*se::PerspectiveRH(se::Pi/3,16.0f/9,0.1f,100.0f)*se::Translation({0,0,-5});
    const auto frustum=se::BuildFrustum(clip);Check(bool(frustum),"meshlet frustum");
    se::MeshletView view;view.position={0,0,-5};view.frustum=*frustum;view.coneCulling=true;
    se::MeshletCullResult result;se::CullMeshlets(set,view,result);
    Check(result.visible.size()+result.culled==set.clusters.size(),"meshlet cull covers every cluster");
    if(result.coneCulled==0)
        std::cerr<<"meshlet diagnostic: coneValid="<<coneValid<<" visible="<<result.visible.size()
            <<" culled="<<result.culled<<" coneCulled="<<result.coneCulled<<"\n";
    Check(result.coneCulled>0,"meshlet cone culling removes back-facing clusters");
    std::uint32_t falseCulls=0;
    for(std::uint32_t cluster=0;cluster<set.clusters.size();++cluster)
    {
        if(std::find(result.visible.begin(),result.visible.end(),cluster)!=result.visible.end())continue;
        const auto& entry=set.clusters[cluster];
        bool visible=false;
        for(std::size_t index=0;index<entry.indices.size()&&!visible;index+=3)
        {
            const auto a=se::Vec3{mesh.vertices[entry.vertices[entry.indices[index]]].position[0],
                mesh.vertices[entry.vertices[entry.indices[index]]].position[1],mesh.vertices[entry.vertices[entry.indices[index]]].position[2]};
            const auto b=se::Vec3{mesh.vertices[entry.vertices[entry.indices[index+1]]].position[0],
                mesh.vertices[entry.vertices[entry.indices[index+1]]].position[1],mesh.vertices[entry.vertices[entry.indices[index+1]]].position[2]};
            const auto c=se::Vec3{mesh.vertices[entry.vertices[entry.indices[index+2]]].position[0],
                mesh.vertices[entry.vertices[entry.indices[index+2]]].position[1],mesh.vertices[entry.vertices[entry.indices[index+2]]].position[2]};
            const auto center=(a+b+c)/3.0f;
            bool inside=true;
            for(const auto& plane:frustum->planes)
                if(plane.x*center.x+plane.y*center.y+plane.z*center.z+plane.d<0){inside=false;break;}
            if(!inside)continue;
            const auto normal=se::Cross(b-a,c-a);
            const auto facing=se::Dot(normal,center-view.position);
            if(facing<-1e-6f)visible=true;
        }
        if(visible)++falseCulls;
    }
    Check(falseCulls==0,"meshlet culling never drops a front-facing triangle inside the frustum");
    se::MeshletSet large;large.sourceVertexCount=set.sourceVertexCount;large.triangleCount=0;
    for(std::uint32_t repeat=0;repeat<453;++repeat)
    {
        for(const auto& cluster:set.clusters){large.clusters.push_back(cluster);large.triangleCount+=static_cast<std::uint32_t>(cluster.indices.size()/3);}
    }
    const auto started=std::chrono::steady_clock::now();
    for(int iteration=0;iteration<20;++iteration)se::CullMeshlets(large,view,result);
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()/20.0;
    Check(large.triangleCount>=1000000,"meshlet stress set has more than one million triangles");
    std::cout<<"Meshlets: "<<set.clusters.size()<<" clusters ("<<coneValid<<" cone-valid), coverage="<<triangles
        <<" triangles, cull "<<result.visible.size()<<"/"<<large.clusters.size()
        <<", cone culled="<<result.coneCulled<<", stress "<<large.triangleCount<<" tris in "<<elapsed<<" ms\n";
}
void CascadeShadows()
{
    // G08 原型：3 级 CSM 的级划分、包围球、光空间 texel 取整、级选择与 2 级降级。
    const auto clip=*se::PerspectiveRH(se::Pi/3,16.0f/9,0.1f,100.0f);
    se::CascadeShadowSettings settings;
    Check(se::PrepareCascadeShadows(clip,0.1f,100.0f,{0.3f,0.5f,0.8f},3,2048,settings),"cascade prepare");
    Check(settings.cascadeCount==3&&settings.resolution==2048,"cascade count and resolution");
    Check(std::abs(settings.splitDistances[0]-10.09f)<0.3f&&std::abs(settings.splitDistances[1]-35.08f)<0.5f
        &&std::abs(settings.splitDistances[2]-100.0f)<0.01f,"cascade splits are 10/35/100 m");
    for(std::uint32_t cascade=0;cascade<3;++cascade)
    {
        Check(se::IsFinite(settings.clipFromWorld[cascade])&&settings.sphereRadius[cascade]>0,"cascade matrix and sphere");
        const auto& center=settings.sphereCenter[cascade];
        const auto project=settings.clipFromWorld[cascade]*se::Vec4{center.x,center.y,center.z,1};
        Check(std::abs(project.w)>1e-6f,"cascade center w");
        const auto x=(project.x/project.w*0.5f+0.5f)*settings.resolution;
        const auto y=(project.y/project.w*0.5f+0.5f)*settings.resolution;
        Check(std::abs(x-std::round(x))<0.02f&&std::abs(y-std::round(y))<0.02f,"cascade center snapped to texel grid");
    }
    auto selection=se::SelectCascade(settings,5.0f);
    Check(selection.first==0&&selection.second==0&&selection.blend==0,"near depth uses cascade 0");
    selection=se::SelectCascade(settings,9.5f);
    Check(selection.first==0&&selection.second==1&&selection.blend>0&&selection.blend<1,"cascade boundary blends 0 and 1");
    selection=se::SelectCascade(settings,25.0f);
    Check(selection.first==1&&selection.second==1,"middle depth uses cascade 1");
    selection=se::SelectCascade(settings,35.0f);
    Check(selection.first==1&&selection.second==2&&selection.blend>0&&selection.blend<1,"far boundary blends 1 and 2");
    selection=se::SelectCascade(settings,120.0f);
    Check(selection.first==2&&selection.second==2,"beyond far clamps to the last cascade");
    se::CascadeShadowSettings two;
    Check(se::PrepareCascadeShadows(clip,0.1f,100.0f,{0.3f,0.5f,0.8f},2,1536,two),"cascade degradation prepare");
    Check(two.cascadeCount==2&&two.resolution==1536&&std::abs(two.splitDistances[1]-35.08f)<0.5f
        &&se::IsFinite(two.clipFromWorld[1]),"cascade 3 to 2 degradation");
    auto preserved=settings;
    Check(!se::PrepareCascadeShadows(clip,0.1f,100.0f,{0,0,0},3,2048,preserved)&&preserved.cascadeCount==3,
        "invalid light preserves previous cascades");
    Check(!se::PrepareCascadeShadows(clip,100.0f,0.1f,{0.3f,0.5f,0.8f},3,2048,preserved),"invalid near/far rejected");
    Check(!se::PrepareCascadeShadows(clip,0.1f,100.0f,{0.3f,0.5f,0.8f},4,2048,preserved),"four cascades rejected");
    std::cout<<"Cascades: splits "<<settings.splitDistances[0]<<"/"<<settings.splitDistances[1]<<"/"
        <<settings.splitDistances[2]<<", texel-snapped 3 cascades, selection and 3->2 degradation passed\n";
}
void LightClusters()
{
    // G09 原型：16×9×24 指数 Z 集群、灯计数/前缀和/索引、容量溢出回退与 ≤8 灯语义。
    se::LightClusterProjection matrices;matrices.view=se::Mat4{};matrices.clipFromView=*se::PerspectiveRH(se::Pi/3,16.0f/9,0.1f,100.0f);
    std::vector<se::rhi::PointLight> lights;
    lights.push_back({{0,0,-5},{1,1,1},2});
    lights.push_back({{0,0,-20},{1,1,1},3});
    lights.push_back({{2,0,-10},{1,1,1},1.5f});
    for(std::uint32_t index=0;index<5;++index)
        lights.push_back({{float(index%3)-1,float(index%2)-0.5f,-float(8+index*4)},{1,1,1},2});
    se::LightClusterData data;
    Check(se::BuildLightClusters(lights,matrices,0.1f,100.0f,data),"light clusters build");
    Check(data.clusters.size()==se::LightClusterCount&&!data.overflow&&!data.indices.empty(),"light cluster grid and indices");
    std::uint64_t total=0;
    for(const auto& cluster:data.clusters)total+=cluster.count;
    Check(total==data.indices.size()&&data.maxPerCluster<=lights.size(),"light cluster prefix ranges");
    std::uint32_t center=0;
    Check(se::LightClusterIndex(0,0,5,0.1f,100.0f,center),"light cluster index for near center");
    bool nearLight=false;
    for(std::uint32_t index=0;index<data.clusters[center].count;++index)
        if(data.indices[data.clusters[center].offset+index]==0)nearLight=true;
    Check(nearLight,"near center cluster contains the near light");
    std::uint32_t first=0,last=0;
    Check(se::LightClusterZSlice(0.05f,0.1f,100.0f,first)&&first==0,"depth inside near is z slice 0");
    Check(se::LightClusterZSlice(99.0f,0.1f,100.0f,last)&&last==se::LightClusterZ-1,"far depth is the last z slice");
    se::LightClusterData overflow;
    Check(se::BuildLightClusters(lights,matrices,0.1f,100.0f,overflow,1)&&overflow.overflow
        &&overflow.clusters.size()==se::LightClusterCount&&overflow.indices.empty(),"capacity overflow falls back globally");
    se::LightClusterData global;se::FillLightClustersGlobal(8,global);
    Check(global.clusters[100].count==8&&global.clusters[100].offset==0&&!global.overflow,"global light fallback list");
    auto preserved=data;auto bad=matrices;bad.view.values[0]=std::numeric_limits<float>::quiet_NaN();
    Check(!se::BuildLightClusters(lights,bad,0.1f,100.0f,preserved)&&preserved.clusters.size()==se::LightClusterCount,
        "invalid view matrix preserves previous clusters");
    std::cout<<"Light clusters: "<<se::LightClusterCount<<" clusters, "<<lights.size()<<" lights, "
        <<data.indices.size()<<" indices, max "<<data.maxPerCluster<<" per cluster, overflow/global fallback passed\n";
}
void AmbientOcclusion()
{
    // G10 原型：GTAO 只在几何遮挡处变暗、平面保持 1；接触阴影沿光方向命中；双边升采样不越边。
    const auto makeFrame=[](std::uint32_t width,std::uint32_t height,float depth)
    {
        se::ScreenSpaceFrame frame;frame.width=width;frame.height=height;
        frame.viewDepth.assign(std::size_t{width}*height,depth);frame.viewNormal.assign(frame.viewDepth.size(),{0,0,1});
        return frame;
    };
    auto flat=makeFrame(64,64,5.0f);
    se::GtaoSettings gtao;gtao.radius=1.0f;gtao.intensity=1.0f;gtao.slices=4;gtao.steps=8;
    std::vector<float> ao;
    Check(se::ComputeGtao(flat,gtao,ao),"GTAO flat plane");
    const auto flatMinimum=*std::min_element(ao.begin(),ao.end());
    Check(flatMinimum>=0.99f,"GTAO leaves a flat plane unoccluded");
    auto step=makeFrame(64,64,5.0f);
    for(std::uint32_t y=0;y<64;++y)for(std::uint32_t x=0;x<32;++x)
    {
        step.viewDepth[std::size_t{y}*64+x]=3.0f;step.viewNormal[std::size_t{y}*64+x]={1,0,0};
    }
    Check(se::ComputeGtao(step,gtao,ao),"GTAO step frame");
    const auto nearWall=ao[std::size_t{32}*64+33],farFloor=ao[std::size_t{32}*64+60];
    Check(nearWall<0.99f&&farFloor>=0.99f,"GTAO darkens only near the step edge");
    se::ContactShadowSettings contact;contact.length=2.0f;contact.steps=12;contact.bias=0.02f;contact.fade=0.5f;
    auto occluder=makeFrame(64,64,5.0f);
    for(std::uint32_t y=10;y<26;++y)for(std::uint32_t x=20;x<44;++x)occluder.viewDepth[std::size_t{y}*64+x]=4.0f;
    std::vector<float> visibility;
    Check(se::ComputeContactShadows(occluder,contact,{0,1,0},visibility),"contact shadow above the floor");
    Check(visibility[std::size_t{30}*64+30]<0.9f&&visibility[std::size_t{60}*64+60]>0.99f,
        "contact shadow darkens under the occluder only");
    auto half=makeFrame(32,32,5.0f);std::vector<float> halfAo;
    Check(se::ComputeGtao(half,gtao,halfAo),"half resolution GTAO");
    std::vector<float> upsampled;
    Check(se::UpsampleBilateral(halfAo,32,32,flat,upsampled)&&upsampled.size()==ao.size()
        &&*std::min_element(upsampled.begin(),upsampled.end())>=0.99f,"bilateral upsample keeps a flat plane unoccluded");
    std::vector<float> composed;
    se::ComposeScreenSpaceOcclusion(std::vector<float>{0.5f,1.0f},std::vector<float>{0.5f,1.0f},composed);
    Check(composed.size()==2&&std::abs(composed[0]-0.25f)<1e-6f&&std::abs(composed[1]-1.0f)<1e-6f,
        "AO and contact visibility compose multiplicatively");
    std::vector<float> preserved{7.0f};auto invalid=flat;invalid.width=0;
    Check(!se::ComputeGtao(invalid,gtao,preserved)&&preserved==std::vector<float>{7.0f},"invalid GTAO frame preserves output");
    Check(!se::ComputeGtao(flat,{0.0f,1.0f,0.0f,4,8},preserved),"invalid GTAO radius rejected");
    gtao.radius=1.0f;gtao.intensity=1.0f;
    std::vector<float> repeat;
    Check(se::ComputeGtao(step,gtao,repeat)&&repeat==ao,"GTAO is deterministic");
    std::cout<<"Screen-space AO: flat min "<<flatMinimum<<", step near-wall "<<nearWall<<", contact under occluder "
        <<visibility[std::size_t{30}*64+30]<<", bilateral upsample and composition passed\n";
}
void AtmosphereFog()
{
    // G11 原型：透射率 LUT 只在参数变化时重建；天空单次散射；高度雾解析积分。
    se::AtmosphereSettings settings;
    settings.sunDirection=se::Normalize(se::Vec3{0.5f,0.8f,0.2f});
    se::Atmosphere atmosphere;
    Check(atmosphere.Rebuild(settings)&&atmosphere.Ready()&&atmosphere.RebuildCount()==1,"atmosphere rebuild");
    Check(atmosphere.Rebuild(settings)&&atmosphere.RebuildCount()==1,"same atmosphere settings reuse the LUT");
    auto changed=settings;changed.sunDirection=se::Normalize(se::Vec3{0,1,0});
    Check(atmosphere.Rebuild(changed)&&atmosphere.RebuildCount()==2,"changed sun rebuilds the LUT");
    Check(atmosphere.TransmittanceToSun(0,1.0f)>atmosphere.TransmittanceToSun(0,0.1f),"higher sun has more transmittance");
    Check(atmosphere.TransmittanceToSun(50000,0.5f)>atmosphere.TransmittanceToSun(0,0.5f),"higher altitude has more transmittance");
    Check(atmosphere.TransmittanceToSun(0,-0.5f)<1e-3f,"sun below the horizon is blocked");
    const auto zenith=atmosphere.SkyRadiance({0,1,0}),horizon=atmosphere.SkyRadiance({1,0.05f,0});
    Check(se::IsFinite(zenith)&&se::IsFinite(horizon)&&zenith.y>=0&&horizon.y>=0,"sky radiance is finite and non-negative");
    Check(se::Length(zenith-horizon)>1e-6f,"zenith and horizon radiance differ");
    se::Atmosphere noon,sunset;
    auto noonSettings=settings;noonSettings.sunDirection={0,1,0};
    auto sunsetSettings=settings;sunsetSettings.sunDirection=se::Normalize(se::Vec3{1,0.05f,0});
    Check(noon.Rebuild(noonSettings)&&sunset.Rebuild(sunsetSettings),"noon and sunset LUTs");
    Check(noon.SkyRadiance({0,1,0}).y>sunset.SkyRadiance({0,1,0}).y,"noon zenith is brighter than sunset");
    auto preserved=atmosphere;(void)preserved;auto invalid=settings;invalid.sunDirection={0,0,0};
    Check(!atmosphere.Rebuild(invalid)&&atmosphere.Ready(),"invalid atmosphere preserves the previous LUT");
    se::FogSettings fog;fog.density=0.01f;fog.heightFalloff=0;fog.color={1,1,1};
    Check(std::abs(se::FogTransmittance(fog,0,100)-std::exp(-1.0f))<1e-3f,"homogeneous fog matches the analytic transmittance");
    se::FogSettings heightFog;heightFog.density=0.01f;heightFog.heightFalloff=100.0f;
    Check(se::FogTransmittance(heightFog,100,100)>se::FogTransmittance(heightFog,0,100),"higher camera keeps more transmittance");
    float transmittance=0;
    Check(se::IntegrateFog(fog,{0,0,-1},0,100,transmittance).y>0&&transmittance<1,"fog in-scattering is positive");
    fog.density=0;
    const auto clear=se::IntegrateFog(fog,{0,0,-1},0,100,transmittance);
    Check(transmittance==1&&se::Length(clear)<1e-6f,"zero fog density is clear");
    std::cout<<"Atmosphere/fog: LUT rebuilds="<<atmosphere.RebuildCount()<<", zenith "<<zenith.y<<", fog T(100m)="
        <<se::FogTransmittance(se::FogSettings{0.01f,0,0,0,{1,1,1}},0,100)<<" passed\n";
}
void Reflections()
{
    // G12 原型：OBB box projection、最近 2 探针混合、GGX 预滤波与屏幕空间反射。
    se::Vec3 direction;
    Check(se::BoxProjectReflection({0,0,0},{2,1,1},{0,0,0},{1,0,0},direction)
        &&std::abs(direction.x-1.0f)<1e-4f,"box projection along +x");
    Check(se::BoxProjectReflection({0,0,0},{2,1,1},{0,0,0},{0,1,0},direction)
        &&std::abs(direction.y-1.0f)<1e-4f,"box projection along +y");
    Check(se::BoxProjectReflection({0,0,0},{2,1,1},{3,0,0},{-1,0,0},direction)
        &&direction.x<-0.99f,"box projection from outside the box");
    Check(!se::BoxProjectReflection({0,0,0},{0,1,1},{0,0,0},{1,0,0},direction),"degenerate probe box rejected");
    std::vector<se::ReflectionProbe> probes(2);
    probes[0].center={0,0,0};probes[0].radius=1;probes[0].blendDistance=1;
    probes[0].width=4;probes[0].height=2;probes[0].environment.assign(8,{1,0,0});
    probes[1].center={2,0,0};probes[1].radius=1;probes[1].blendDistance=1;
    probes[1].width=4;probes[1].height=2;probes[1].environment.assign(8,{0,0,1});
    se::Vec3 color;std::uint32_t first=0,second=0;float blend=0;
    Check(se::BlendReflectionProbes(probes,{0,0,0},{0,0,1},color,first,second,blend)&&first==0&&second==0
        &&blend==0,"inside one radius uses the nearest probe");
    Check(se::BlendReflectionProbes(probes,{1.5f,0,0},{0,0,1},color,first,second,blend)
        &&first==1&&second==0&&blend>0&&blend<1,"between two probes blends them");
    Check(!se::BlendReflectionProbes(probes,{100,0,0},{0,0,1},color,first,second,blend),"outside every probe falls back");
    std::vector<se::Vec3> constant(8*4,se::Vec3{0.5f,0.6f,0.7f}),filtered;
    Check(se::PrefilterGgx(constant,8,4,1.0f,64,filtered)&&filtered.size()==constant.size(),"GGX prefilter constant environment");
    bool constantPreserved=true;
    for(const auto& value:filtered)if(se::Length(value-constant[0])>1e-4f)constantPreserved=false;
    Check(constantPreserved,"GGX prefilter preserves a constant environment");
    std::vector<se::Vec3> gradient(8*4);
    for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<8;++x)gradient[std::size_t{y}*8+x]={float(x)/8,float(y)/4,0.5f};
    std::vector<se::Vec3> sharp,blurred;
    Check(se::PrefilterGgx(gradient,8,4,0.0f,32,sharp)&&se::PrefilterGgx(gradient,8,4,1.0f,32,blurred),"GGX prefilter roughness levels");
    float sharpMean=0,blurredMean=0;
    for(std::size_t index=0;index<sharp.size();++index){sharpMean+=sharp[index].x;blurredMean+=blurred[index].x;}
    sharpMean/=float(sharp.size());blurredMean/=float(blurred.size());
    float sharpVariance=0,blurredVariance=0;
    for(std::size_t index=0;index<sharp.size();++index)
    {
        const auto sharpDelta=sharp[index].x-sharpMean,blurredDelta=blurred[index].x-blurredMean;
        sharpVariance+=sharpDelta*sharpDelta;blurredVariance+=blurredDelta*blurredDelta;
    }
    Check(sharpVariance>blurredVariance,"higher roughness blurs the environment more");
    const auto makeFrame=[](float depth)
    {
        se::ScreenSpaceFrame frame;frame.width=64;frame.height=64;
        frame.viewDepth.assign(64*64,depth);frame.viewNormal.assign(64*64,{0,0,1});
        return frame;
    };
    auto frame=makeFrame(5.0f);
    for(std::uint32_t y=10;y<30;++y)for(std::uint32_t x=20;x<44;++x)frame.viewDepth[std::size_t{y}*64+x]=4.0f;
    se::SsrSettings ssr;ssr.steps=32;ssr.thickness=0.05f;ssr.bias=0.02f;ssr.maxDistance=10.0f;
    se::SsrHit hit;
    Check(se::TraceScreenSpaceReflection(frame,ssr,30,40,{0,1,0},hit)&&hit.hit&&hit.u>=0&&hit.u<=1&&hit.confidence>0,
        "SSR hits the occluder");
    auto empty=makeFrame(5.0f);
    Check(se::TraceScreenSpaceReflection(empty,ssr,30,40,{0,1,0},hit)&&!hit.hit,"SSR misses when no geometry is behind");
    Check(se::TraceScreenSpaceReflection(frame,ssr,60,30,{1,0,0},hit)&&!hit.hit,"SSR misses when the ray leaves the screen");
    Check(!se::TraceScreenSpaceReflection(frame,{0,0.05f,0.02f,10},30,40,{0,1,0},hit),"SSR rejects zero steps");
    Check(!se::TraceScreenSpaceReflection(frame,{0,0.05f,0.02f,10},30,40,{0,1,0},hit),"SSR rejects zero steps");
}
void TemporalAA()
{
    // Halton(2,3) 8 相位：范围、周期与互异性。
    std::array<TaaJitter,8> jitter{};
    for(std::uint32_t index=0;index<jitter.size();++index)
    {
        jitter[index]=HaltonJitter(index);
        Check(jitter[index].x>=-0.5f&&jitter[index].x<=0.5f&&jitter[index].y>=-0.5f&&jitter[index].y<=0.5f,
            "TAA jitter stays inside one pixel");
        const auto repeated=HaltonJitter(index+8);
        Check(repeated.x==jitter[index].x&&repeated.y==jitter[index].y,"TAA jitter repeats every eight frames");
    }
    Check(jitter[0].x==0.0f,"TAA frame zero has no horizontal jitter");
    for(std::size_t first=0;first<jitter.size();++first)
        for(std::size_t second=first+1;second<jitter.size();++second)
            Check(jitter[first].x!=jitter[second].x||jitter[first].y!=jitter[second].y,"TAA jitter phases are distinct");
    // 抖动进入裁剪空间：NDC 偏移 = 2*pixel/尺寸。
    const auto projection=*PerspectiveRH(Pi/3,16.0f/9.0f,0.1f,100.0f);
    const auto jittered=ApplyProjectionJitter(projection,{0.25f,-0.5f},1920,1080);
    const Vec4 point{0,0,-5,1};
    const auto baseline=projection*point,shifted=jittered*point;
    Check(std::abs(shifted.x/shifted.w-(baseline.x/baseline.w+2.0f*0.25f/1920))<1e-6f
        &&std::abs(shifted.y/shifted.w-(baseline.y/baseline.w-2.0f*0.5f/1080))<1e-6f,
        "TAA projection jitter shifts clip coordinates by the pixel offset");
    Check(ApplyProjectionJitter(projection,{0.25f,0.25f},0,1080).values==projection.values,
        "invalid viewport rejects TAA jitter atomically");
    // 重投影：静止为 0，相机左移使当前 UV 右移（du<0），相机后方无效。
    const auto clip=*PerspectiveRH(Pi/3,16.0f/9.0f,0.1f,100.0f)*Translation({0,0,-3});
    const auto previous=*PerspectiveRH(Pi/3,16.0f/9.0f,0.1f,100.0f)*Translation({-0.1f,0,-3});
    const auto still=ReprojectVelocity(clip,clip,{0,0,0});
    Check(still.valid&&still.du==0.0f&&still.dv==0.0f,"static camera/object reprojects to zero velocity");
    const auto moved=ReprojectVelocity(clip,previous,{0,0,0});
    Check(moved.valid&&moved.du<0.0f&&std::abs(moved.du)>1e-4f,"camera translation produces signed screen velocity");
    Check(!ReprojectVelocity(clip,previous,{0,0,5}).valid,"point behind the camera has no valid velocity");
    // YCoCg 往返与邻域夹取。
    const Vec3 source{0.31f,0.72f,0.19f};
    const auto roundtrip=YCoCgToRgb(RgbToYCoCg(source));
    Check(std::abs(roundtrip.x-source.x)<1e-6f&&std::abs(roundtrip.y-source.y)<1e-6f&&std::abs(roundtrip.z-source.z)<1e-6f,
        "YCoCg transform roundtrips");
    std::array<Vec3,9> neighborhood{};
    for(std::size_t index=0;index<neighborhood.size();++index)
        neighborhood[index]=Vec3{0.5f,0.5f,0.5f}+Vec3{float(int(index%3)-1),float(int(index/3)-1),float(int(index%2))*0.5f-0.25f}*0.03f;
    Vec3 minimum{},maximum{};
    YCoCgNeighborhood(neighborhood,minimum,maximum);
    const auto clamped=ClampToNeighborhood({0.9f,0.1f,0.1f},neighborhood);
    const auto clampedYCoCg=RgbToYCoCg(clamped);
    Check(clampedYCoCg.x>=minimum.x-1e-6f&&clampedYCoCg.x<=maximum.x+1e-6f
        &&clampedYCoCg.y>=minimum.y-1e-6f&&clampedYCoCg.y<=maximum.y+1e-6f
        &&clampedYCoCg.z>=minimum.z-1e-6f&&clampedYCoCg.z<=maximum.z+1e-6f,
        "TAA history is clamped into the YCoCg neighborhood box");
    // 静止收敛：新帧权重 0.1 -> 历史权重 0.9，60 帧后误差显著下降。
    TaaSettings settings;settings.staticBlend=0.1f;settings.dynamicBlend=0.2f;
    const auto sameColor=[](Vec3 first,Vec3 second){return Length(first-second)<1e-6f;};
    TaaResolveInput input;input.neighborhood=neighborhood;input.history={0.54f,0.52f,0.48f};
    input.historyValid=true;input.velocity={true,0,0};input.currentDepth=input.historyDepth=0.3f;
    const auto initialError=Length(input.history-neighborhood[4]);
    Vec3 history=input.history;
    for(int frame=0;frame<60;++frame)
    {
        input.history=history;
        const auto resolved=ResolveTaa(input,settings);
        Check(!resolved.disoccluded&&std::abs(resolved.historyWeight-0.9f)<1e-5f,"static TAA blends 90% history");
        history=resolved.color;
    }
    Check(Length(history-neighborhood[4])<initialError*0.01f,"static TAA converges over 60 frames");
    // 深度/速度/透明/关闭的失效与回退。
    auto invalid=input;invalid.historyDepth=0.9f;
    auto result=ResolveTaa(invalid,settings);
    Check(result.disoccluded&&result.historyWeight==0.0f&&sameColor(result.color,input.neighborhood[4]),"depth jump disoccludes history");
    invalid=input;invalid.velocity={true,0.05f,0};
    result=ResolveTaa(invalid,settings);
    Check(result.disoccluded&&sameColor(result.color,input.neighborhood[4]),"velocity above threshold disoccludes history");
    invalid=input;invalid.transparentOrUi=true;
    result=ResolveTaa(invalid,settings);
    Check(result.historyWeight==0.0f&&sameColor(result.color,input.neighborhood[4]),"transparent/UI bypasses TAA history");
    auto disabled=settings;disabled.enabled=false;
    result=ResolveTaa(input,disabled);
    Check(sameColor(result.color,input.neighborhood[4])&&result.historyWeight==0.0f,"disabled TAA returns the current frame for FXAA fallback");
    // 历史 ping-pong：resize/场景切换/暂停恢复都失效。
    TaaHistory taaHistory;
    Check(!taaHistory.Usable(64,64),"TAA history starts invalid");
    taaHistory.Resize(64,64);taaHistory.Confirm();
    Check(taaHistory.Usable(64,64)&&!taaHistory.Usable(32,64),"TAA history is dimension-locked");
    taaHistory.Invalidate();Check(!taaHistory.Usable(64,64),"scene cut invalidates TAA history");
    taaHistory.Confirm();Check(taaHistory.Usable(64,64),"confirming a valid size re-arms history");
    taaHistory.Resize(32,32);Check(!taaHistory.Usable(32,32)&&taaHistory.Width()==32,"resize invalidates TAA history");
    std::cout<<"TAA: 8 jitter phases, static 60-frame error="<<Length(history-neighborhood[4])
        <<" (initial "<<initialError<<"), velocity/disocclusion/history invalidation passed\n";
}
void AutoExposureTests()
{
    ExposureSettings settings;
    // 直方图：像素计数、桶范围与分位亮度。
    std::vector<float> uniform(1000,0.18f);
    auto histogram=BuildExposureHistogram(uniform,settings);
    const auto total=std::accumulate(histogram.bins.begin(),histogram.bins.end(),std::uint64_t{0});
    Check(histogram.pixels==1000&&total==1000,"exposure histogram counts every finite pixel");
    const auto median=PercentileLuminance(histogram,0.5f,settings);
    Check(std::abs(std::log2(median/0.18f))<0.04f,"uniform luminance maps back to its bucket within quantization");
    std::vector<float> mixed;
    mixed.insert(mixed.end(),250,0.05f);mixed.insert(mixed.end(),750,0.5f);
    histogram=BuildExposureHistogram(mixed,settings);
    const auto low=PercentileLuminance(histogram,settings.lowPercentile,settings);
    const auto high=PercentileLuminance(histogram,settings.highPercentile,settings);
    Check(low<=high&&low>=0.05f*0.9f&&high<=0.5f*1.1f,"60/80 percentiles stay inside the scene luminance range");
    const auto targetLuminance=TargetLuminance(histogram,settings);
    Check(targetLuminance>=low*0.999f&&targetLuminance<=high*1.001f,"exposure target is between the configured percentiles");
    Check(BuildExposureHistogram({},settings).pixels==0,"empty exposure histogram is valid but empty");
    // 暗→亮阶跃：目标约为 0.18/0.09 = 2，亮适应 3/s。
    AutoExposure exposure;exposure.Reset(1.0f);
    std::vector<float> neutral(100,0.18f),dark(100,0.09f),bright(100,1.8f),black(100,0.0f),white(100,1e6f);
    float output=0;
    Check(exposure.Update(BuildExposureHistogram(neutral,settings),1.0f/60.0f,settings,output)
        &&std::abs(output-1.0f)<0.02f,"neutral scene initializes exposure near one");
    for(int frame=0;frame<60;++frame)exposure.Update(BuildExposureHistogram(dark,settings),1.0f/60.0f,settings,output);
    const auto darkExposure=output;
    Check(output>1.85f&&output<2.05f,"dark-to-bright step exposes near 0.18/0.09 after one second");
    exposure.Reset(1.0f);
    exposure.Update(BuildExposureHistogram(neutral,settings),1.0f/60.0f,settings,output);
    for(int frame=0;frame<60;++frame)exposure.Update(BuildExposureHistogram(bright,settings),1.0f/60.0f,settings,output);
    const auto brightExposure=output;
    Check(output>0.40f&&output<0.46f,"bright-to-dark step uses the slower dark adaptation");
    // 全黑/全白：钳制而不是震荡或发散。
    exposure.Reset(1.0f);exposure.Update(BuildExposureHistogram(neutral,settings),1.0f/60.0f,settings,output);
    float previous=output;bool monotonic=true;
    for(int frame=0;frame<120;++frame)
    {
        exposure.Update(BuildExposureHistogram(black,settings),1.0f/60.0f,settings,output);
        monotonic&=output>=previous-1e-6f;previous=output;
        Check(std::isfinite(output)&&output<=settings.maximumExposure,"all-black exposure stays finite and clamped");
    }
    exposure.Reset(1.0f);exposure.Update(BuildExposureHistogram(neutral,settings),1.0f/60.0f,settings,output);
    previous=output;monotonic=true;
    for(int frame=0;frame<120;++frame)
    {
        exposure.Update(BuildExposureHistogram(white,settings),1.0f/60.0f,settings,output);
        monotonic&=output<=previous+1e-6f;previous=output;
        Check(std::isfinite(output)&&output>=settings.minimumExposure,"all-white exposure stays finite and clamped");
    }
    // 非有限/空直方图/异常 dt 不改变状态。
    const auto stable=exposure.Adapted();
    float ignored=0;
    Check(!exposure.Update({},1.0f/60.0f,settings,ignored)&&ignored==stable,"empty histogram keeps the adapted exposure");
    std::vector<float> invalid{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()};
    Check(!exposure.Update(BuildExposureHistogram(invalid,settings),1.0f/60.0f,settings,ignored),"non-finite luminance produces no histogram samples");
    std::cout<<"Exposure: dark->bright="<<darkExposure<<", bright->dark="<<brightExposure
        <<", all-black/all-white clamped, 256 bins passed\n";
}
void ColorGradingTests(const std::filesystem::path& output)
{
    // ACES/Reinhard 基础形状与单调性。
    const auto black=TonemapAcesFitted({0,0,0});
    Check(black.x==0&&black.y==0&&black.z==0,"ACES maps black to black");
    const auto reference=TonemapReinhard({1,1,1});
    Check(std::abs(reference.x-0.5f)<1e-6f,"Reinhard remains available as a reference");
    float previous=0;
    for(const float value:{0.0f,0.01f,0.1f,0.5f,1.0f,4.0f,16.0f})
    {
        const auto mapped=TonemapAcesFitted({value,value,value});
        Check(mapped.x>=previous&&mapped.x<=1.0f&&std::abs(mapped.y-mapped.x)<1e-5f
            &&std::abs(mapped.z-mapped.x)<1e-5f,"ACES is monotonic and per-channel uniform");
        previous=mapped.x;
    }
    // sRGB 精确往返与单次编码。
    for(const float value:{0.0f,0.001f,0.0031308f,0.05f,0.5f,0.9f,1.0f})
    {
        const auto encoded=LinearToSrgb({value,value,value});
        const auto decoded=SrgbToLinear(encoded);
        Check(std::abs(decoded.x-value)<2e-5f,"sRGB encode/decode roundtrips without double gamma");
    }
    ColorGradingSettings neutral;
    const Vec3 hdr{1.7f,0.8f,0.25f};
    const auto direct=LinearToSrgb(TonemapAcesFitted(hdr));
    const auto present=PresentSrgb(hdr,neutral);
    Check(Length(present-direct)<1e-6f,"present applies sRGB exactly once after ACES");
    Check(Length(ApplyLiftGammaGain(hdr,neutral)-hdr)<1e-6f,"neutral lift/gamma/gain is identity");
    // lift/gamma/gain 的确定性效果与 LUT。
    auto gained=neutral;gained.gain[0]=2.0f;
    Check(std::abs(ApplyLiftGammaGain({0.2f,0.2f,0.2f},gained).x-0.4f)<1e-6f,"gain scales linear color before encoding");
    auto gamma=neutral;gamma.gamma[0]=2.0f;
    Check(std::abs(ApplyLiftGammaGain({0.25f,0.25f,0.25f},gamma).x-0.5f)<1e-6f,"gamma applies the reciprocal exponent");
    ColorLut lut;
    Check(lut.Build(33,neutral)&&lut.Ready(),"neutral 33^3 LUT builds");
    const auto corner=lut.Sample({0.0f,0.5f,1.0f});
    Check(std::abs(corner.x-0.0f)<1e-5f&&std::abs(corner.y-0.5f)<1e-5f&&std::abs(corner.z-1.0f)<1e-5f,
        "LUT samples exact lattice points");
    auto withLut=neutral;withLut.lutEnabled=true;
    const auto lutGraded=GradeColor(hdr,withLut,&lut);
    const auto directGraded=TonemapAcesFitted(hdr);
    Check(std::abs(lutGraded.x-directGraded.x)<1e-3f&&std::abs(lutGraded.y-directGraded.y)<1e-3f
        &&std::abs(lutGraded.z-directGraded.z)<1e-3f,"neutral LUT matches ACES direct within trilinear error");
    withLut.lutEnabled=false;
    const auto bypass=GradeColor(hdr,withLut,&lut);
    Check(Length(bypass-directGraded)<1e-6f,"disabling the LUT is an exact ACES bypass");
    // 灰阶/色卡：无死黑、过曝或 NaN。
    for(const float value:{0.0f,0.01f,0.1f,0.5f,1.0f,4.0f,16.0f})
    {
        const auto srgb=PresentSrgb({value,value,value},neutral);
        Check(srgb.x>=0.0f&&srgb.x<=1.0f&&std::abs(srgb.y-srgb.x)<1e-5f&&std::abs(srgb.z-srgb.x)<1e-5f
            &&std::isfinite(srgb.x),"gray ramp stays displayable");
    }
    for(const Vec3 chart: {Vec3{0.9f,0.2f,0.1f},Vec3{0.1f,0.8f,0.2f},Vec3{0.1f,0.2f,0.9f},Vec3{0.5f,0.5f,0.5f}})
    {
        const auto srgb=PresentSrgb(chart,neutral);
        Check(srgb.x>=0.0f&&srgb.x<=1.0f&&srgb.y>=0.0f&&srgb.y<=1.0f&&srgb.z>=0.0f&&srgb.z<=1.0f,"color chart stays displayable");
    }
    // UI 在 tonemap 后合成：alpha=1 完全等于 UI 颜色，不随曝光变化。
    const Vec3 ui{0.2f,0.5f,0.8f};
    const auto first=ComposeUi({0.4f,0.5f,0.6f},ui,1.0f,0.0625f,neutral);
    const auto second=ComposeUi({8.0f,9.0f,10.0f},ui,1.0f,4.0f,neutral);
    Check(Length(first-ui)<1e-5f&&Length(second-ui)<1e-5f,
        "full-alpha UI color is independent of scene exposure");
    const auto halfDark=ComposeUi({0.4f,0.5f,0.6f},ui,0.5f,0.0625f,neutral);
    const auto halfBright=ComposeUi({8.0f,9.0f,10.0f},ui,0.5f,4.0f,neutral);
    const auto backgroundDark=GradeColor(Vec3{0.4f,0.5f,0.6f}*0.0625f,neutral);
    const auto backgroundBright=GradeColor(Vec3{8.0f,9.0f,10.0f}*4.0f,neutral);
    const auto halfDarkLinear=SrgbToLinear(halfDark),halfBrightLinear=SrgbToLinear(halfBright);
    Check(std::abs((halfDarkLinear.x-halfBrightLinear.x)-0.5f*(backgroundDark.x-backgroundBright.x))<1e-5f,
        "half-alpha UI keeps a constant linear contribution");
    // G15：.selut 版本/色彩空间标记。线性 LUT 允许 >1 的显示值；sRGB 标记要求可编码值域，
    // 加载时统一解码到显示线性；未知版本/色彩空间/截断/超范围都会拒绝。
    std::string assetError;
    ColorLut linearAsset;Check(linearAsset.Build(33,gained),"linear LUT builds for .selut");
    const auto linearPath=output/"color-grade-linear.selut";
    Check(linearAsset.SaveAsset(linearPath,ColorLutColorSpace::Linear,assetError),"linear .selut writes");
    ColorLut linearLoaded;Check(linearLoaded.LoadAsset(linearPath,assetError),"linear .selut reads");
    Check(linearLoaded.Size()==33&&linearLoaded.AssetColorSpace()==ColorLutColorSpace::Linear,
        ".selut keeps version/size and linear color-space marker");
    for(std::uint32_t red=0;red<33;red+=8)for(std::uint32_t green=0;green<33;green+=8)for(std::uint32_t blue=0;blue<33;blue+=8)
    {
        const Vec3 coordinate{float(red)/32.0f,float(green)/32.0f,float(blue)/32.0f};
        const auto expected=linearAsset.Sample(coordinate),actual=linearLoaded.Sample(coordinate);
        Check(std::abs(actual.x-expected.x)<1e-6f&&std::abs(actual.y-expected.y)<1e-6f
            &&std::abs(actual.z-expected.z)<1e-6f,"linear .selut roundtrips lattice values");
    }
    ColorLutAsset rawAsset;
    Check(ReadColorLutAsset(linearPath,rawAsset,assetError)&&rawAsset.size==33
        &&rawAsset.colorSpace==ColorLutColorSpace::Linear&&rawAsset.values.size()==33u*33u*33u,
        ".selut header exposes size and color-space marker");
    const auto srgbPath=output/"color-grade-srgb.selut";
    Check(lut.SaveAsset(srgbPath,ColorLutColorSpace::Srgb,assetError),"sRGB .selut writes");
    Check(ReadColorLutAsset(srgbPath,rawAsset,assetError)&&rawAsset.colorSpace==ColorLutColorSpace::Srgb,
        "sRGB color-space marker roundtrips");
    ColorLut srgbLoaded;Check(srgbLoaded.LoadAsset(srgbPath,assetError)&&srgbLoaded.AssetColorSpace()==ColorLutColorSpace::Srgb,
        "sRGB .selut decodes once into display-linear values");
    for(const Vec3 coordinate: {Vec3{0,0,0},Vec3{0.25f,0.5f,0.75f},Vec3{1,1,1}})
    {
        const auto expected=lut.Sample(coordinate),actual=srgbLoaded.Sample(coordinate);
        Check(std::abs(actual.x-expected.x)<2e-5f&&std::abs(actual.y-expected.y)<2e-5f
            &&std::abs(actual.z-expected.z)<2e-5f,"sRGB .selut roundtrips within encode/decode tolerance");
    }
    const auto readBytes=[&](const std::filesystem::path& path)
    {
        std::ifstream file(path,std::ios::binary);return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),{});
    };
    const auto writeBytes=[&](const std::filesystem::path& path,const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream file(path,std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    };
    const auto bytes=readBytes(linearPath);Check(bytes.size()>24,"read .selut header bytes");
    auto tampered=bytes;tampered[4]=2;const auto versionPath=output/"color-grade-bad-version.selut";writeBytes(versionPath,tampered);
    ColorLutAsset rejected;assetError.clear();
    Check(!ReadColorLutAsset(versionPath,rejected,assetError)&&assetError=="Rendering.LutAssetInvalid",
        ".selut rejects unknown version without replacing output");
    tampered=bytes;tampered[16]=9;const auto spacePath=output/"color-grade-bad-space.selut";writeBytes(spacePath,tampered);assetError.clear();
    Check(!ReadColorLutAsset(spacePath,rejected,assetError)&&assetError=="Rendering.LutAssetInvalid",
        ".selut rejects unknown color-space marker");
    const auto truncatedPath=output/"color-grade-truncated.selut";writeBytes(truncatedPath,std::vector<std::uint8_t>(bytes.begin(),bytes.begin()+24));assetError.clear();
    Check(!ReadColorLutAsset(truncatedPath,rejected,assetError)&&assetError=="Rendering.LutAssetInvalid",
        ".selut rejects truncated payload");
    ColorLutAsset tooWide;tooWide.size=33;tooWide.colorSpace=ColorLutColorSpace::Srgb;tooWide.values.assign(33u*33u*33u,Vec3{0,0,2});
    assetError.clear();Check(!WriteColorLutAsset(output/"color-grade-bad-range.selut",tooWide,assetError)
        &&assetError=="Rendering.LutAssetInvalid",".selut rejects unencodable sRGB values");
    // G15.09：29 号输出布局的 1089×33 strip。Rgba8 饱和到 [0,1]；Rgba16F 以 half 保留 >1 的
    // 显示线性值，两者都按 x=blue*33+red、y=green 排布，alpha 不透明。
    auto hdrGrading=neutral;hdrGrading.gain[0]=2.0f;hdrGrading.gain[1]=1.0f;hdrGrading.gain[2]=0.5f;
    ColorLut hdrLut;Check(hdrLut.Build(33,hdrGrading),"HDR LUT for strip bake builds");
    ColorLutStrip strip8,strip16;
    Check(BakeColorLutStrip(hdrLut,ColorLutStripFormat::Rgba8,strip8)&&strip8.Valid()
        &&strip8.width==1089&&strip8.height==33&&strip8.bytesPerPixel==4
        &&strip8.bytes.size()==std::size_t(1089)*33*4,"Rgba8 strip bakes at 1089x33");
    Check(BakeColorLutStrip(hdrLut,ColorLutStripFormat::Rgba16Float,strip16)&&strip16.Valid()
        &&strip16.width==1089&&strip16.height==33&&strip16.bytesPerPixel==8
        &&strip16.bytes.size()==std::size_t(1089)*33*8,"Rgba16F strip bakes at 1089x33");
    const auto stripOffset=[](const ColorLutStrip& strip,std::uint32_t red,std::uint32_t green,std::uint32_t blue)
    {return (std::size_t(green)*strip.width+blue*33+red)*strip.bytesPerPixel;};
    const auto stripHalf=[&](const ColorLutStrip& strip,std::size_t offset)
    {return HalfToFloat(static_cast<std::uint16_t>(strip.bytes[offset]|(strip.bytes[offset+1]<<8)));};
    const auto red8=stripOffset(strip8,32,0,0),red16=stripOffset(strip16,32,0,0);
    Check(strip8.bytes[red8]==255&&std::abs(stripHalf(strip16,red16)-2.0f)<1e-3f,
        "8-bit strip saturates >1 while Rgba16F keeps the display-linear value");
    Check(strip8.bytes[red8+3]==255&&std::abs(stripHalf(strip16,red16+6)-1.0f)<1e-6f,"strip alpha stays opaque");
    for(const std::uint32_t red:{0u,11u,32u})for(const std::uint32_t green:{0u,17u,32u})for(const std::uint32_t blue:{0u,5u,32u})
    {
        const auto expected=hdrLut.Values()[(std::size_t(red)*33+green)*33+blue];
        const auto offset8=stripOffset(strip8,red,green,blue),offset16=stripOffset(strip16,red,green,blue);
        float expectedChannel[3]{expected.x,expected.y,expected.z};
        for(std::uint32_t channel=0;channel<3;++channel)
        {
            const auto quantized=float(strip8.bytes[offset8+channel])/255.0f;
            const auto half=stripHalf(strip16,offset16+channel*2);
            Check(std::abs(quantized-std::clamp(expectedChannel[channel],0.0f,1.0f))<=0.5f/255.0f+1e-6f,
                "Rgba8 strip quantizes lattice values to 8 bits");
            Check(std::abs(half-expectedChannel[channel])<=std::max(1e-3f,std::abs(expectedChannel[channel])*0.001f),
                "Rgba16F strip retains half precision lattice values");
        }
    }
    ColorLut emptyLut;const auto stripBytesBefore=strip16.bytes.size();
    Check(!BakeColorLutStrip(emptyLut,ColorLutStripFormat::Rgba16Float,strip16)&&strip16.bytes.size()==stripBytesBefore
        &&strip16.Valid(),"strip bake rejects an empty LUT without replacing output");
    std::cout<<"Color: ACES(1)="<<TonemapAcesFitted({1,1,1}).x
        <<", neutral 33^3 LUT error<1e-3, sRGB roundtrip, UI constant, .selut markers, 8/16-bit strips passed\n";
}
void ColorLutHotReload(const std::filesystem::path& output)
{
    const auto path=output/"color-lut-hot-reload.selut";
    const auto writeLut=[&](const ColorGradingSettings& grading,ColorLutColorSpace space)
    {
        ColorLut lut;
        Check(lut.Build(33,grading),"hot reload fixture builds");
        std::string error;
        Check(lut.SaveAsset(path,space,error),"hot reload fixture writes");
        // 同一进程内连续覆盖时文件时间可能落在同一刻度；显式推进时间戳，
        // 保证测试验证的是“文件变化触发重载”，而不是依赖文件系统分辨率。
        std::error_code ignored;
        const auto stamp=std::filesystem::last_write_time(path,ignored);
        if(!ignored)std::filesystem::last_write_time(path,stamp+std::chrono::seconds(1),ignored);
        return lut;
    };
    ColorGradingSettings warm;warm.gain[0]=1.15f;warm.gain[1]=1.0f;warm.gain[2]=0.85f;
    const auto first=writeLut(warm,ColorLutColorSpace::Linear);
    std::string error;
    ColorLutFile file(0.0f);
    Check(file.Open(path,error)&&file.Ready(),"LUT file opens");
    Check(file.Status().revision==1&&file.Status().size==33&&file.Status().reloads==0,
        "LUT file reports the first load");
    const auto values=file.Values();
    Check(values&&values->size()==33u*33u*33u,"LUT file exposes an immutable lattice snapshot");
    const auto sampleAt=[&](const ColorLut& lut){return lut.Sample({0.11f,0.52f,0.93f});};
    const auto firstSample=sampleAt(file.Lut());
    Check(!file.Poll()&&file.Status().revision==1,"unchanged file does not reload");
    Check(file.Poll(true)&&file.Status().revision==2&&file.Status().reloads==1,
        "forced poll reloads the current file");
    // 写入不同的 LUT；非 force 轮询检测到时间戳变化后整体替换。
    ColorGradingSettings cool;cool.lift[0]=0.05f;cool.lift[1]=0.02f;cool.lift[2]=0.0f;
    cool.gamma[0]=0.9f;cool.gamma[1]=1.0f;cool.gamma[2]=1.1f;
    const auto second=writeLut(cool,ColorLutColorSpace::Linear);
    Check(file.Poll()&&file.Status().revision==3&&file.Status().reloads==2,"changed file hot reloads");
    const auto secondSample=sampleAt(file.Lut());
    Check(std::abs(secondSample.x-firstSample.x)>1e-4f||std::abs(secondSample.z-firstSample.z)>1e-4f,
        "hot reload replaces graded values");
    const auto secondExpected=sampleAt(second);
    Check(std::abs(secondSample.x-secondExpected.x)<1e-6f&&std::abs(secondSample.z-secondExpected.z)<1e-6f,
        "hot reload matches the newly written LUT");
    // 写坏文件：保留旧数据与 index，只更新错误状态；再次写好后自动恢复。
    {
        std::ofstream broken(path,std::ios::binary|std::ios::trunc);
        broken.write("SELT\x01\x00\x00\x00",8);
    }
    const auto corruptStamp=std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path,corruptStamp+std::chrono::seconds(1));
    Check(!file.Poll()&&file.Status().error=="Rendering.LutAssetInvalid"&&file.Status().failures==1,
        "broken file reports a stable error key");
    Check(file.Ready()&&file.Status().revision==3,"broken file keeps the last valid LUT");
    const auto kept=sampleAt(file.Lut());
    Check(std::abs(kept.x-secondExpected.x)<1e-6f&&std::abs(kept.z-secondExpected.z)<1e-6f,
        "broken file does not replace the last valid lattice");
    ColorGradingSettings third;third.gain[0]=0.8f;third.gain[1]=0.9f;third.gain[2]=1.0f;
    const auto recovered=writeLut(third,ColorLutColorSpace::Srgb);
    Check(file.Poll()&&file.Status().revision==4&&file.Status().error.empty()
        &&file.Status().colorSpace==ColorLutColorSpace::Srgb,"repaired file reloads with its marker");
    Check(file.Lut().AssetColorSpace()==ColorLutColorSpace::Srgb,"repaired sRGB LUT keeps its source marker");
    const auto recoveredSample=sampleAt(file.Lut()),recoveredExpected=sampleAt(recovered);
    Check(std::abs(recoveredSample.x-recoveredExpected.x)<2e-5f
        &&std::abs(recoveredSample.z-recoveredExpected.z)<2e-5f,
        "sRGB hot reload decodes once into display-linear values");
    // 删除文件：保留最近成功的数据，下一次出现时重新加载。
    std::error_code ignored;
    std::filesystem::remove(path,ignored);
    Check(!file.Poll()&&file.Status().error=="Assets.ReadFailed"&&file.Status().revision==4,
        "missing file keeps the current LUT");
    writeLut(warm,ColorLutColorSpace::Linear);
    Check(file.Poll()&&file.Status().revision==5&&file.Status().error.empty(),
        "reappearing file reloads again");
    file.Close();
    Check(!file.Ready()&&!file.Values(),"closing the LUT file releases its snapshot");
    std::cout<<"Color LUT hot reload: initial load, unchanged skip, forced reload, changed/broken/repaired/deleted, .selut markers passed\n";
}
void FrameGraphs(const std::filesystem::path& output)
{
    // 基础：版本解析与稳定顺序。
    {
        FrameGraph graph;
        const auto color=graph.AddTexture("Color",{8,8,1,rhi::TextureFormat::Rgba16Float},true);
        const auto depth=graph.AddTexture("Depth",{8,8,1,rhi::TextureFormat::Rgba8,true},true);
        const auto back=graph.AddTexture("Back",{8,8,1,rhi::TextureFormat::Rgba8},true);
        const auto clear=graph.AddPass({"Clear",rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite});
        graph.Write(clear,color);graph.Write(clear,depth);
        const auto sky=graph.AddPass({"Sky",rhi::PassKind::Sky,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None});
        graph.Read(sky,color);graph.Write(sky,color);
        const auto opaque=graph.AddPass({"Opaque",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,rhi::ResourceUse::DepthRead});
        graph.Read(opaque,color);graph.Read(opaque,depth);graph.Write(opaque,color);graph.Write(opaque,depth);
        const auto present=graph.AddPass({"Output",rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,true});
        graph.Read(present,color);graph.Write(present,back);
        Check(graph.Compile()==FrameGraphStatus::Success,"frame graph basic compile");
        const auto passes=graph.Passes();
        Check(passes.size()==4&&passes[0].name=="Clear"&&passes[1].name=="Sky"&&passes[2].name=="Opaque"&&passes[3].name=="Output",
            "frame graph keeps declaration order without dependencies");
        Check(passes[1].reads.size()==1&&passes[1].reads[0].version==1&&passes[2].reads.size()==2&&passes[2].reads[1].version==1,
            "frame graph pins reads to the current produced version");
        Check(passes[2].writes.size()==2&&passes[2].writes[0].version==3&&passes[3].writes[0].version==1,
            "frame graph increments versions per write");
        Check(graph.Stats().edges>=6&&graph.Stats().executedPasses==4&&graph.Stats().culledPasses==0,"frame graph dependency edges");
        Check(graph.ExecutionOrder().size()==4,"frame graph execution order view");
    }
    // 未定义读：瞬态资源在首次写入前被读取。
    {
        FrameGraph graph;
        const auto texture=graph.AddTexture("NeverWritten",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto pass=graph.AddPass({"ReadOnly",rhi::PassKind::Opaque});
        graph.Read(pass,texture);
        Check(graph.Compile()==FrameGraphStatus::UndefinedRead&&graph.LastError().find("ReadOnly")!=std::string_view::npos,
            "frame graph rejects undefined transient read");
    }
    // 同一 Pass 重复写同一资源。
    {
        FrameGraph graph;
        const auto texture=graph.AddTexture("Twice",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto pass=graph.AddPass({"DoubleWrite",rhi::PassKind::Opaque});
        graph.Write(pass,texture);graph.Write(pass,texture);
        Check(graph.Compile()==FrameGraphStatus::DuplicateWrite&&graph.LastError().find("DoubleWrite")!=std::string_view::npos,
            "frame graph rejects two writes in one pass");
    }
    // 禁用 Pass 后读者回退到更早版本，被禁用的 Pass 不进入执行顺序。
    {
        FrameGraph graph;
        const auto color=graph.AddTexture("Color",{4,4,1,rhi::TextureFormat::Rgba8},true);
        const auto ldr=graph.AddTexture("Ldr",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto back=graph.AddTexture("Back",{4,4,1,rhi::TextureFormat::Rgba8},true);
        const auto clear=graph.AddPass({"Clear",rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite});
        graph.Write(clear,color);
        const auto blur=graph.AddPass({"Blur",rhi::PassKind::ToneMap,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead});
        graph.Read(blur,color);graph.Write(blur,color);
        const auto tone=graph.AddPass({"Tone",rhi::PassKind::ToneMap,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead});
        graph.Read(tone,color);graph.Write(tone,ldr);
        const auto present=graph.AddPass({"Output",rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,true});
        graph.Read(present,ldr);graph.Write(present,back);
        graph.SetPassEnabled(blur,false);
        Check(graph.Compile()==FrameGraphStatus::Success,"frame graph disabled pass compiles");
        Check(graph.Stats().disabledPasses==1&&graph.Passes().size()==3,"disabled pass leaves execution order");
        Check(graph.Passes()[1].reads.size()==1&&graph.Passes()[1].reads[0].version==1,
            "consumer of a disabled write falls back to the earlier version");
    }
    // 环检测：两个 Pass 互相读取对方稍后写入的版本。
    {
        FrameGraph graph;
        const auto x=graph.AddTexture("X",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto y=graph.AddTexture("Y",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto a=graph.AddPass({"A",rhi::PassKind::Opaque});
        graph.Write(a,x);graph.ReadVersion(a,{y,1});
        const auto b=graph.AddPass({"B",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,rhi::ResourceUse::None,false,true});
        graph.Write(b,y);graph.ReadVersion(b,{x,1});
        Check(graph.Compile()==FrameGraphStatus::Cycle&&graph.LastError().find("A")!=std::string_view::npos
            &&graph.LastError().find("B")!=std::string_view::npos,"frame graph rejects dependency cycle");
    }
    // 显式前向版本：消费者先声明，生产者后声明，拓扑排序仍把生产者放到前面。
    {
        FrameGraph graph;
        const auto x=graph.AddTexture("X",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto y=graph.AddTexture("Y",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto consumer=graph.AddPass({"Consumer",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,
            rhi::ResourceUse::None,false,true});
        graph.ReadVersion(consumer,{x,1});graph.ReadVersion(consumer,{y,1});
        const auto writerY=graph.AddPass({"WriteY",rhi::PassKind::Opaque});graph.Write(writerY,y);
        const auto writerX=graph.AddPass({"WriteX",rhi::PassKind::Opaque});graph.Write(writerX,x);
        Check(graph.Compile()==FrameGraphStatus::Success,"frame graph forward version compiles");
        const auto passes=graph.Passes();
        Check(passes.size()==3&&passes[0].name=="WriteY"&&passes[1].name=="WriteX"&&passes[2].name=="Consumer",
            "frame graph topological sort reorders producers before a forward-reading consumer");
    }
    // 无输出且无副作用的 Pass 链整体剔除。
    {
        FrameGraph graph;
        const auto color=graph.AddTexture("Color",{4,4,1,rhi::TextureFormat::Rgba8},true);
        const auto back=graph.AddTexture("Back",{4,4,1,rhi::TextureFormat::Rgba8},true);
        const auto dead=graph.AddTexture("Dead",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto dead2=graph.AddTexture("Dead2",{4,4,1,rhi::TextureFormat::Rgba8});
        const auto first=graph.AddPass({"DeadA",rhi::PassKind::Opaque});graph.Write(first,dead);
        const auto second=graph.AddPass({"DeadB",rhi::PassKind::Opaque});graph.Read(second,dead);graph.Write(second,dead2);
        const auto clear=graph.AddPass({"Clear",rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite});graph.Write(clear,color);
        const auto present=graph.AddPass({"Output",rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,true});
        graph.Read(present,color);graph.Write(present,back);
        Check(graph.Compile()==FrameGraphStatus::Success&&graph.Passes().size()==2&&graph.Stats().culledPasses==2,
            "frame graph culls passes without outputs or side effects");
    }
    // 别名：同描述、生命周期不重叠的瞬态纹理复用槽位并统计节省；重叠或描述不同不复用。
    {
        FrameGraph graph;
        const auto back=graph.AddTexture("Back",{4,4,1,rhi::TextureFormat::Rgba8},true);
        const auto a=graph.AddTexture("AliasA",{100,50,1,rhi::TextureFormat::Rgba8});
        const auto b=graph.AddTexture("AliasB",{100,50,1,rhi::TextureFormat::Rgba8});
        const auto c=graph.AddTexture("AliasC",{100,50,1,rhi::TextureFormat::Rgba8});
        const auto writeA=graph.AddPass({"WriteA",rhi::PassKind::Opaque});graph.Write(writeA,a);
        const auto readA=graph.AddPass({"ReadA",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,rhi::ResourceUse::None,false,true});
        graph.Read(readA,a);
        const auto writeB=graph.AddPass({"WriteB",rhi::PassKind::Opaque});graph.Write(writeB,b);
        const auto readB=graph.AddPass({"ReadB",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,rhi::ResourceUse::None,false,true});
        graph.Read(readB,b);
        const auto writeC=graph.AddPass({"WriteC",rhi::PassKind::Opaque});graph.Write(writeC,c);
        const auto readC=graph.AddPass({"ReadC",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,rhi::ResourceUse::None,false,true});
        graph.Read(readC,c);
        const auto present=graph.AddPass({"Output",rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,true});
        graph.Read(present,c);graph.Write(present,back);
        Check(graph.Compile()==FrameGraphStatus::Success,"frame graph alias compile");
        std::uint32_t groups[3]{FrameGraphInvalid,FrameGraphInvalid,FrameGraphInvalid};
        for(const auto& texture:graph.Textures())
        {
            if(texture.name=="AliasA")groups[0]=texture.aliasGroup;
            if(texture.name=="AliasB")groups[1]=texture.aliasGroup;
            if(texture.name=="AliasC")groups[2]=texture.aliasGroup;
        }
        Check(groups[0]!=FrameGraphInvalid&&groups[0]==groups[1]&&groups[1]==groups[2],
            "disjoint same-description transient textures share an alias slot");
        Check(graph.Stats().savedBytes==40000&&graph.Stats().transientBytes==60000&&graph.Stats().aliasedBytes==20000,
            "frame graph reports alias bytes separately from total transient bytes");
    }
    {
        FrameGraph graph;
        const auto back=graph.AddTexture("Back",{4,4,1,rhi::TextureFormat::Rgba8},true);
        const auto a=graph.AddTexture("OverlapA",{64,64,1,rhi::TextureFormat::Rgba8});
        const auto b=graph.AddTexture("OverlapB",{64,64,1,rhi::TextureFormat::Rgba8});
        const auto c=graph.AddTexture("Different",{64,64,1,rhi::TextureFormat::Rgba16Float});
        const auto write=graph.AddPass({"Write",rhi::PassKind::Opaque});graph.Write(write,a);graph.Write(write,b);graph.Write(write,c);
        const auto read=graph.AddPass({"Read",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,rhi::ResourceUse::None,false,true});
        graph.Read(read,a);graph.Read(read,b);graph.Read(read,c);
        const auto present=graph.AddPass({"Output",rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,true});
        graph.Read(present,c);graph.Write(present,back);
        Check(graph.Compile()==FrameGraphStatus::Success&&graph.Stats().aliasGroups==3&&graph.Stats().savedBytes==0,
            "overlapping lifetimes and different descriptions never share an alias slot");
    }
    // 60 Pass 性能：包含声明、校验、拓扑排序、生命周期与别名，目标 < 0.1 ms。
    {
        constexpr std::uint32_t passCount=60,iterations=200;
        FrameGraph graph(passCount+8,passCount+8,1024,16);
        std::array<std::uint32_t,passCount> textures{};
        std::array<std::string,passCount> textureNames{},passNames{};
        for(std::uint32_t index=0;index<passCount;++index)
        {
            textureNames[index]="T"+std::to_string(index);
            passNames[index]="P"+std::to_string(index);
        }
        const auto build=[&]{
            graph.Reset();
            for(std::uint32_t index=0;index<passCount;++index)
                textures[index]=graph.AddTexture(textureNames[index],{64,64,1,rhi::TextureFormat::Rgba8});
            for(std::uint32_t index=0;index<passCount;++index)
            {
                const auto pass=graph.AddPass({passNames[index],rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,
                    rhi::ResourceUse::None,false,index+1==passCount});
                if(index)graph.Read(pass,textures[index-1]);
                graph.Write(pass,textures[index]);
            }
        };
        build();Check(graph.Compile()==FrameGraphStatus::Success&&graph.Passes().size()==passCount,"frame graph compiles a 60-pass chain");
        std::vector<double> timings;timings.reserve(iterations);
        for(std::uint32_t iteration=0;iteration<iterations;++iteration)
        {
            const auto start=std::chrono::steady_clock::now();
            build();
            const auto status=graph.Compile();
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            Check(status==FrameGraphStatus::Success,"frame graph 60-pass compile");
            timings.push_back(elapsed);
        }
        std::sort(timings.begin(),timings.end());
        const auto median=timings[timings.size()/2],minimum=timings.front(),maximum=timings.back();
        Check(median<0.1,"frame graph 60-pass build under 0.1 ms");
        Check(graph.Stats().compileMilliseconds<0.1&&graph.Stats().declaredPasses==passCount,"frame graph internal timing agrees");
        std::cout<<"FrameGraph 60 passes: median="<<median<<" ms, min="<<minimum<<" ms, max="<<maximum<<" ms, aliases="
            <<graph.Stats().aliasGroups<<", saved="<<graph.Stats().savedBytes<<" B\n";
    }
    // 截断与重复编译：只跑前 N 个 Pass 是确定性的调试视图，不改变声明。
    {
        FrameGraph graph;
        const auto color=graph.AddTexture("Color",{4,4,1,rhi::TextureFormat::Rgba8},true);
        const auto back=graph.AddTexture("Back",{4,4,1,rhi::TextureFormat::Rgba8},true);
        const auto clear=graph.AddPass({"Clear",rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite});graph.Write(clear,color);
        const auto sky=graph.AddPass({"Sky",rhi::PassKind::Sky,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None});graph.Read(sky,color);graph.Write(sky,color);
        const auto opaque=graph.AddPass({"Opaque",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite});graph.Read(opaque,color);graph.Write(opaque,color);
        const auto present=graph.AddPass({"Output",rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,true});graph.Read(present,color);graph.Write(present,back);
        graph.SetPassLimit(2);
        Check(graph.Compile()==FrameGraphStatus::Success&&graph.Passes().size()==2&&graph.Stats().clipped,"frame graph pass limit truncates execution");
        graph.SetPassLimit(0);
        Check(graph.Compile()==FrameGraphStatus::Success&&graph.Passes().size()==4&&!graph.Stats().clipped,"frame graph recompiles without the debug limit");
        Check(graph.WriteDot(output/"frame-graph.dot")&&graph.WriteJson(output/"frame-graph.json"),"frame graph DOT/JSON export");
        std::ifstream dot(output/"frame-graph.dot"),json(output/"frame-graph.json");std::string dotText((std::istreambuf_iterator<char>(dot)),{}),jsonText((std::istreambuf_iterator<char>(json)),{});
        Check(dotText.find("digraph FrameGraph")!=std::string::npos&&dotText.find("Output")!=std::string::npos
            &&jsonText.find("\"passes\"")!=std::string::npos&&jsonText.find("\"resources\"")!=std::string::npos,
            "frame graph debug exports contain passes and resources");
    }
    // 容量上限：超过帧 arena 声明容量的操作在编译前就返回 CapacityExceeded。
    {
        FrameGraph graph(1,1,4,1);
        const auto first=graph.AddTexture("First",{4,4,1,rhi::TextureFormat::Rgba8});
        Check(first!=FrameGraphInvalid&&graph.AddTexture("Second",{4,4,1,rhi::TextureFormat::Rgba8})==FrameGraphInvalid,
            "frame graph rejects resources beyond capacity");
        Check(graph.Compile()==FrameGraphStatus::CapacityExceeded,"frame graph reports capacity failure");
    }
}
void ImpostorAtlases(const std::filesystem::path& output)
{
    // G05.12：16 视角 4×4 图集、alpha 覆盖、视角选择与 .seimp 往返。
    auto cube=se::MakeUnitCube();
    se::ImpostorBakeSettings settings;settings.tileSize=32;settings.color={0.85f,0.35f,0.1f};
    se::ImpostorAtlas atlas;std::vector<se::AssetDiagnostic> diagnostics;
    Check(se::BakeImpostorAtlas(cube,se::Mat4{},settings,atlas,diagnostics),"impostor bake");
    Check(atlas.tileSize==32&&atlas.columns==4&&atlas.rows==4&&atlas.Width()==128&&atlas.Height()==128,
        "impostor atlas is a 4x4 tile grid");
    Check(atlas.rgba.size()==std::size_t(atlas.Width())*atlas.Height()*4,"impostor atlas pixel count");
    std::uint64_t covered=0,differingViews=0;
    for(std::uint32_t view=0;view<se::ImpostorViewCount;++view)
    {
        std::uint64_t tileCovered=0;
        for(std::uint32_t y=0;y<atlas.tileSize;++y)for(std::uint32_t x=0;x<atlas.tileSize;++x)
            tileCovered+=atlas.rgba[atlas.PixelOffset(view,x,y)+3]>0;
        Check(tileCovered>0,"every impostor view has coverage");
        Check(atlas.rgba[atlas.PixelOffset(view,16,16)+3]==255,"impostor view covers its center");
        covered+=tileCovered;
    }
    for(std::uint32_t y=0;y<atlas.tileSize;++y)for(std::uint32_t x=0;x<atlas.tileSize;++x)
    {
        const auto first=atlas.PixelOffset(0,x,y),second=atlas.PixelOffset(2,x,y);
        differingViews+=std::abs(int(atlas.rgba[first])-int(atlas.rgba[second]))>2;
    }
    Check(differingViews>0,"azimuth 45 degree view differs from the front view");
    const auto path=output/"cube.seimp";std::string error;
    Check(se::WriteImpostorAtlas(path,atlas,error),"write .seimp");
    se::ImpostorAtlas loaded;
    Check(se::ReadImpostorAtlas(path,loaded,error)&&loaded.tileSize==32&&loaded.rgba==atlas.rgba,"read .seimp round trip");
    std::vector<std::uint8_t> bytes;Check(se::asset_io::Read(path,bytes)&&bytes.size()>16,"read .seimp bytes");
    bytes.resize(bytes.size()-1);
    {std::ofstream out(path,std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    Check(!se::ReadImpostorAtlas(path,loaded,error),"truncated .seimp rejected");
    Check(se::ImpostorViewIndex(0)==0&&se::ImpostorViewIndex(se::Pi/8)==1,"impostor nearest view index");
    std::uint32_t first=0,second=0;float blend=0;
    se::ImpostorViewPair(0,first,second,blend);
    Check(first==0&&second==1&&blend==0,"impostor view pair starts at the front view");
    se::ImpostorViewPair(se::Pi/16,first,second,blend);
    Check(first==0&&second==1&&std::abs(blend-0.5f)<1e-6f,"impostor view pair interpolates the half step");
    se::ImpostorViewPair(-se::Pi/16,first,second,blend);
    Check(first==15&&second==0&&std::abs(blend-0.5f)<1e-6f,"impostor view pair wraps around");
    std::cout<<"Impostor atlas: "<<se::ImpostorViewCount<<" views, tile="<<atlas.tileSize
        <<", covered_pixels="<<covered<<", differing_45deg="<<differingViews<<"\n";
}
void HiZSamples(const std::filesystem::path&)
{
    // G06.01：CPU Hi-Z 归约与保守遮挡测试（薄物体/近平面/相机在包围盒内均不误剔）。
    constexpr std::uint32_t width=64,height=64;
    std::vector<float> depth(static_cast<std::size_t>(width)*height,1.0f);
    // 中心 32×32 的遮挡面，深度 0.05。
    for(std::uint32_t y=16;y<48;++y)for(std::uint32_t x=16;x<48;++x)depth[std::size_t(y)*width+x]=0.05f;
    se::HiZBuffer hzb;
    Check(hzb.Build(width,height,depth),"Hi-Z build");
    Check(hzb.MipCount()==7&&hzb.Mip(0).width==64&&hzb.Mip(6).width==1,"Hi-Z mip chain is 64..1");
    const auto mipCount=hzb.MipCount();
    Check(std::abs(hzb.Mip(1).depth[std::size_t(8)*32+8]-0.05f)<1e-6f
        &&std::abs(hzb.Mip(1).depth[0]-1.0f)<1e-6f,"Hi-Z 2x2 max reduction");
    Check(hzb.MaxDepth(0,20,20)==0.05f&&hzb.MaxDepth(0,0,0)==1.0f,"Hi-Z depth sampling");
    const auto clip=*se::OrthographicRH(8,6,0.1f,100);
    const auto box=[](float x,float y,float z,float halfX,float halfY,float halfZ)
    {return se::Aabb{{x-halfX,y-halfY,z-halfZ},{x+halfX,y+halfY,z+halfZ}};};
    const auto behind=se::TestOcclusion(hzb,box(0,0,-10,1,1,0.5f),clip,width,height);
    Check(behind.occluded,"small box behind the occluder is occluded");
    const auto front=se::TestOcclusion(hzb,box(0,0,-2,1,1,0.5f),clip,width,height);
    Check(!front.occluded,"box in front of the occluder stays visible");
    const auto edge=se::TestOcclusion(hzb,box(3.5f,0,-10,1,1,0.5f),clip,width,height);
    Check(!edge.occluded,"box crossing the occluder edge is conservative");
    const auto full=se::TestOcclusion(hzb,box(0,0,-10,4,3,0.5f),clip,width,height);
    Check(!full.occluded&&full.mip==5,"full screen box samples the coarse mip and is conservative");
    // 薄遮挡：背景 1.0 中只有一条 1 px 深 0.02 的线，粗 mip 会取到背景 1.0，不能判遮挡。
    std::vector<float> thin(static_cast<std::size_t>(width)*height,1.0f);
    for(std::uint32_t x=16;x<48;++x)thin[std::size_t(32)*width+x]=0.02f;
    se::HiZBuffer thinHzb;Check(thinHzb.Build(width,height,thin),"thin Hi-Z build");
    const auto thinResult=se::TestOcclusion(thinHzb,box(0,0,-10,1,1,0.5f),clip,width,height);
    Check(!thinResult.occluded,"thin occluder never rejects a box behind it");
    // 近平面穿插与相机在包围盒内：投影失败即保留。
    const auto nearPlane=se::TestOcclusion(hzb,box(0,0,-0.05f,1,1,0.2f),clip,width,height);
    Check(!nearPlane.occluded,"box crossing the near plane stays visible");
    const auto inside=se::TestOcclusion(hzb,box(0,0,0,1,1,2),clip,width,height);
    Check(!inside.occluded,"camera inside the box stays visible");
    const auto offscreen=se::TestOcclusion(hzb,box(100,0,-10,1,1,0.5f),clip,width,height);
    Check(!offscreen.occluded,"offscreen box is not rejected by Hi-Z");
    se::ScreenRect rect;Check(se::ProjectScreenRect(box(0,0,-10,1,1,0.5f),clip,width,height,rect)&&rect.Valid(),
        "screen rect projection");
    Check(!se::ProjectScreenRect(box(0,0,10,1,1,0.5f),clip,width,height,rect),"behind-camera box has no screen rect");
    Check(!hzb.Build(0,8,{})&&!hzb.Build(2,2,std::vector<float>(3,0.5f)),"Hi-Z rejects invalid sizes");
    // G06.06：HZB 调试可视化。近处遮挡面应比背景亮，mip 级数与尺寸与 CPU 参考一致。
    // 上面的非法尺寸用例会清空 hzb，这里用同一份深度重建一份只读参考。
    se::HiZBuffer previewHzb;Check(previewHzb.Build(width,height,depth),"Hi-Z preview source");
    se::HiZPreview preview;Check(se::BuildHiZPreview(previewHzb,preview),"Hi-Z preview builds");
    Check(preview.Ready()&&preview.width==width&&preview.height==height&&preview.levels==mipCount
        &&preview.levelInfo.size()==mipCount&&preview.levelInfo[0].width==width
        &&preview.levelInfo[mipCount-1].width==1,"Hi-Z preview reports every mip size");
    const auto center=preview.mip0[(std::size_t(32)*width+32)*4];
    const auto corner=preview.mip0[0];
    Check(center>corner&&center>=240&&corner<=4,"Hi-Z preview maps near depth to white and background to black");
    Check(preview.levelInfo[0].minimumDepth==0.05f&&preview.levelInfo[0].maximumDepth==1.0f,
        "Hi-Z preview keeps per-level depth range");
    Check(!se::BuildHiZPreview(se::HiZBuffer{},preview)&&!preview.Ready(),"Hi-Z preview rejects an empty chain");
    std::cout<<"Hi-Z: mips="<<mipCount<<", occluded_case=1, conservative_cases=6, preview_center="
        <<int(center)<<", preview_corner="<<int(corner)<<"\n";
}
void HiZGating(const std::filesystem::path&)
{
    // G06.02：首帧/resize/FOV>10%/相机 cut 跳过遮挡，其余帧允许复用。
    se::HiZHistory history;
    se::HiZFrameState state;
    state.viewProjection=*se::OrthographicRH(8,6,0.1f,100);
    state.cameraPosition={0,0,3};state.cameraForward={0,0,-1};
    state.verticalFov=se::Pi/3;state.width=64;state.height=64;
    Check(history.Update(state)==se::HiZInvalidation::FirstFrame&&!history.Usable(),"first frame skips occlusion");
    Check(history.Update(state)==se::HiZInvalidation::None&&history.Usable(),"same camera reuses HZB");
    auto resized=state;resized.width=32;
    Check(history.Update(resized)==se::HiZInvalidation::Resize&&!history.Usable(),"resize invalidates HZB");
    Check(history.Update(resized)==se::HiZInvalidation::None,"second frame after resize is usable");
    auto fov=resized;fov.verticalFov=se::Pi/3*1.15f;
    Check(history.Update(fov)==se::HiZInvalidation::FieldOfView,"FOV change above 10% invalidates HZB");
    Check(history.Update(fov)==se::HiZInvalidation::None,"accepted FOV stays usable");
    auto smallFov=fov;smallFov.verticalFov=fov.verticalFov*1.05f;
    Check(history.Update(smallFov)==se::HiZInvalidation::None,"FOV change within 10% stays usable");
    auto moved=smallFov;moved.cameraPosition={30,0,3};
    Check(history.Update(moved)==se::HiZInvalidation::CameraCut,"teleport invalidates HZB");
    auto step=moved;step.cameraPosition={31,0,3};
    Check(history.Update(step)==se::HiZInvalidation::None,"small movement stays usable");
    auto turned=step;turned.cameraForward=se::Normalize(se::Vec3{1,0,0});
    Check(history.Update(turned)==se::HiZInvalidation::CameraCut,"90 degree turn invalidates HZB");
    auto bad=step;bad.cameraPosition={std::numeric_limits<float>::quiet_NaN(),0,0};
    Check(history.Update(bad)==se::HiZInvalidation::CameraCut,"non-finite camera state invalidates HZB");
    history.Reset();
    Check(history.Update(step)==se::HiZInvalidation::FirstFrame&&!history.Usable(),"reset returns to first frame");
    std::cout<<"Hi-Z gating: first/resize/FOV/cut/non-finite rejected, steady/small motion accepted\n";
}
void Contracts(const Localization& text)
{
    WindowState window;Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,320,240),"queue contract window");rhi::Result result;auto device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);
    struct Cleanup{WindowState& window;std::shared_ptr<rhi::Device>& device;~Cleanup(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}} cleanup{window,device};Check(bool(result)&&bool(device),"queue contract device");
    {
        std::vector<std::uint8_t> rgba(4*4*4,200);
        se::Bc1Image bc;std::string encodeError;
        Check(se::CompressBc1(rgba,4,4,bc,encodeError),"BC1 texture fixture");
        rhi::TextureDescription description{4,4};description.format=rhi::TextureFormat::Bc1Unorm;description.name="Test.Bc1";
        const rhi::TextureSubresource subresource{std::span<const std::uint8_t>(bc.blocks),0};
        rhi::Texture bcTexture;
        Check(bool(device->CreateTextureSubresources(description,std::span(&subresource,1),bcTexture)),"BC1 texture upload");
        std::vector<std::uint8_t> mipSource(8*8*4,200);
        std::vector<se::Bc1Image> chain;
        Check(se::CompressBc1MipChain(mipSource,8,8,4,chain,encodeError)&&chain.size()==4,"BC1 mip chain");
        rhi::TextureDescription mipDescription{8,8};mipDescription.mipLevels=4;mipDescription.format=rhi::TextureFormat::Bc1UnormSrgb;
        std::vector<rhi::TextureSubresource> mips;
        for(const auto& image:chain)mips.push_back({std::span<const std::uint8_t>(image.blocks),0});
        rhi::Texture mipTexture;
        Check(bool(device->CreateTextureSubresources(mipDescription,mips,mipTexture)),"BC1 mip chain upload");
        std::vector<std::uint8_t> white(4,255);rhi::Texture plain;
        Check(bool(device->CreateTexture({1,1},white,plain)),"plain texture fixture");
        Check(device->SetTextureMipRange(plain,0).error==rhi::Error::Unsupported,"uncompressed texture rejects mip clamp");
        Check(bool(device->SetTextureMipRange(mipTexture,1))&&bool(device->SetTextureMipRange(mipTexture,3))
            &&bool(device->SetTextureMipRange(mipTexture,0)),"compressed texture mip range round trip");
        Check(device->SetTextureMipRange(mipTexture,4).error==rhi::Error::InvalidArgument,"mip clamp rejects out-of-range level");
        // G04.07：先只上传尾部 mip，再补传更细 mip 并放宽 SRV。
        const rhi::TextureSubresource tail[]{ {std::span<const std::uint8_t>(chain[2].blocks),0},
            {std::span<const std::uint8_t>(chain[3].blocks),0} };
        rhi::Texture partialTexture;
        Check(bool(device->CreateTextureSubresourcesRange(mipDescription,2,tail,partialTexture)),"partial mip texture upload");
        Check(bool(device->SetTextureMipRange(partialTexture,2)),"partial texture clamps to uploaded mips");
        const rhi::TextureSubresource head[]{ {std::span<const std::uint8_t>(chain[0].blocks),0},
            {std::span<const std::uint8_t>(chain[1].blocks),0} };
        Check(bool(device->UploadTextureSubresourcesRange(partialTexture,0,head)),"stream missing mips");
        Check(bool(device->SetTextureMipRange(partialTexture,0)),"full mip clamp after streaming");
        rhi::Texture rejectedRange;
        Check(device->CreateTextureSubresourcesRange(mipDescription,4,tail,rejectedRange).error==rhi::Error::InvalidArgument
            &&!rejectedRange,"partial range rejects out-of-range first mip");
        // G04.08：BC4 上传与 mip 钳制。
        se::Bc4Image ao;Check(se::CompressBc4(rgba,4,4,ao,encodeError),"BC4 texture fixture");
        rhi::TextureDescription aoDescription{4,4};aoDescription.format=rhi::TextureFormat::Bc4Unorm;aoDescription.name="Test.Bc4";
        const rhi::TextureSubresource aoSub{std::span<const std::uint8_t>(ao.blocks),0};
        rhi::Texture bc4Texture;
        Check(bool(device->CreateTextureSubresources(aoDescription,std::span(&aoSub,1),bc4Texture)),"BC4 texture upload");
        Check(bool(device->SetTextureMipRange(bc4Texture,0)),"BC4 texture mip range");
        // G04-AC01：2048² 反照率 BC1 链（2,796,216 B）与显存占用。
        se::ImageData large{2048,2048};large.rgba8.assign(std::size_t(2048)*2048*4,200);
        se::BakedTexture largeBake;
        Check(se::BakeBc1Texture(large,se::BakedTextureFormat::Bc1UnormSrgb,largeBake,encodeError),"2048 albedo bake");
        Check(largeBake.bytes.size()==2796216,"2048 BC1 chain bytes");
        rhi::TextureDescription largeDescription{2048,2048,largeBake.mipLevels,1,
            rhi::TextureFormat::Bc1UnormSrgb,"Test.2048"};
        std::vector<rhi::TextureSubresource> largeMips;
        for(std::uint32_t mip=0;mip<largeBake.mipLevels;++mip)
            largeMips.push_back({std::span<const std::uint8_t>(largeBake.bytes).subspan(largeBake.mipOffsets[mip],
                static_cast<std::size_t>(se::BakedMipBytes(se::BakedTextureFormat::Bc1UnormSrgb,2048,2048,mip))),0});
        rhi::Texture largeTexture;const auto beforeVram=device->Stats().memoryUsedBytes;
        Check(bool(device->CreateTextureSubresources(largeDescription,largeMips,largeTexture)),"2048 BC1 texture upload");
        const auto vram=device->Stats().memoryUsedBytes-beforeVram;
        Check(vram>=largeBake.bytes.size()&&vram<=largeBake.bytes.size()*2,"2048 BC1 vram matches chain size");
        // G05.04：LOD 链上传为每级索引缓冲，绘制按级别选择索引。
        se::StaticModel sphere;sphere.meshes.push_back(se::MakeUnitSphere());
        se::ModelNode sphereNode;sphereNode.name="Sphere";sphereNode.meshes={0};sphere.nodes.push_back(sphereNode);
        auto sphereModel=std::make_shared<se::StaticModel>(std::move(sphere));
        se::StaticModelRenderCache lodCache{true};
        std::shared_ptr<const se::StaticRenderModel> lodModel;std::vector<se::AssetDiagnostic> lodDiagnostics;
        Check(lodCache.Upload(sphereModel,{},std::filesystem::current_path(),*device,lodModel,lodDiagnostics,false,4),
            "LOD model upload");
        Check(lodModel&&lodModel->meshes[0].lod&&lodModel->meshes[0].lodIndices.size()==3,"LOD index buffers uploaded");
        std::vector<se::rhi::DrawIndexed> lodDraws;
        se::AppendPbrModelDraws(*lodModel,{}, {},lodDraws,{},se::Mat4{}, {0,0,3},{0,0,1},{1,1,1},0);
        se::AppendPbrModelDraws(*lodModel,{}, {},lodDraws,{},se::Mat4{}, {0,0,3},{0,0,1},{1,1,1},3);
        Check(lodDraws.size()==2&&lodDraws[0].indices==*lodModel->meshes[0].indices
            &&lodDraws[1].indices==*lodModel->meshes[0].lodIndices[2],"LOD draws select per-level index buffers");
        Check(lodDraws[0].indexCount==lodModel->meshes[0].indexCount
            &&lodDraws[1].indexCount==lodModel->meshes[0].lod->levels[3].indices.size(),"LOD draws report per-level counts");
        // G05.08：过渡绘制的带符号 dither 与级别索引同时保留。
        std::vector<se::rhi::DrawIndexed> ditherDraws;
        se::AppendPbrModelDraws(*lodModel,{}, {},ditherDraws,{},se::Mat4{}, {0,0,3},{0,0,1},{1,1,1},1,-0.5f);
        se::AppendPbrModelDraws(*lodModel,{}, {},ditherDraws,{},se::Mat4{}, {0,0,3},{0,0,1},{1,1,1},2,0.5f);
        Check(ditherDraws.size()==2&&ditherDraws[0].pbr.lodDither==-0.5f&&ditherDraws[1].pbr.lodDither==0.5f,
            "LOD dither draws keep signed blend weights");
        Check(ditherDraws[0].indices==*lodModel->meshes[0].lodIndices[0]
            &&ditherDraws[1].indices==*lodModel->meshes[0].lodIndices[1],"LOD dither draws keep per-level indices");
    }
    auto commands=device->BeginCommands(1);Check(bool(commands),"queue commands");
    Check(commands->BeginPass({rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"InvalidCache",rhi::ResourceUse::None,true}).error==rhi::Error::Unsupported,"cache option rejects unrelated pass");
    Check(bool(commands->BeginPass({rhi::PassKind::Clear}))&&bool(commands->EndPass()),"valid clear after rejected declaration");
    Check(bool(commands->BeginPass({rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"Opaque",rhi::ResourceUse::None,true})),"cache opaque");
    Check(commands->Draw({}).error==rhi::Error::InvalidHandle,"invalid draw still validated");Check(bool(commands->EndPass())&&bool(commands->Close()),"empty valid opaque after failed draw");rhi::CompletionToken token;Check(bool(device->Submit(*commands,token)),"empty queue submit");
    Check(device->Stats().opaqueBindings.constants==0&&device->Stats().opaqueBindings.descriptorTables==0,"no stale counters after empty opaque");
}
}
int wmain(int argc,wchar_t** argv)
{
    try{Check(argc==2,"supply queue output");const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"queue localization");
        Cpu();OcclusionHudVisuals(text);G00Contracts();Telemetry(output);Variants();MaterialInstances();VertexCompression();MeshCache();TexturePipeline(output);LodChains(output);HlodCells(output);HlodStreaming(output);HlodStreamAsync(output);HlodProxyBudget();Meshlets(output);CascadeShadows();LightClusters();AmbientOcclusion();AtmosphereFog();Reflections();TemporalAA();AutoExposureTests();ColorGradingTests(output);ColorLutHotReload(output);FrameGraphs(output);ImpostorAtlases(output);HiZSamples(output);HiZGating(output);Gpu(output);Contracts(text);std::cout<<"RenderQueueTests passed (stable keys, four paths, two frame reuse, resize/revision, actual binding counts, frame graph order/alias/lifetime, and invalid state).\n";return 0;}
    catch(const std::exception& error){std::cerr<<"RenderQueueTests: "<<error.what()<<'\n';return 1;}
}
