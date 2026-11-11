// G15：GPU 自动曝光与 LUT 的真实 compute 验证。
// 直方图逐桶对照 CPU AutoExposure；适应按 60 帧阶跃对照同一公式；LUT 晶格对照
// ColorLut::Build；并覆盖全黑/全白、非有限像素、契约与 1080p 成本。
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Exposure.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Graphics/D3D12/D3D12StateTracker.h"
#include "PlatformLayer/Graphics/D3D12/D3D12UploadArena.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "FunctionLayer/Rendering/AutoExposure.h"
#include "FunctionLayer/Rendering/ColorGrading.h"
#include "FunctionLayer/Rendering/ColorLutFile.h"
#include "CoreLayer/Math/Half.h"
#include "CoreLayer/Localization.h"
#include <d3d12sdklayers.h>
#include <nlohmann/json.hpp>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace se;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
UINT RowPitch(UINT width,UINT bytesPerPixel){return (width*bytesPerPixel+255)&~255u;}
float Luminance(Vec3 color)
{
    return 0.2126f*std::max(color.x,0.0f)+0.7152f*std::max(color.y,0.0f)+0.0722f*std::max(color.z,0.0f);
}
struct Fixture
{
    D3D12Context context;
    D3D12Exposure kernel;
    D3D12UploadArena uploads;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    Microsoft::WRL::Wrappers::Event event;
    UINT64 next=1,fenceValue=0;
    bool stopped=false;
    explicit Fixture(const Localization& text)
    {
        Check(InitializeD3D12Context(context,text),"exposure device");
        D3D12_COMMAND_QUEUE_DESC queueDescription{};queueDescription.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(context.device->CreateCommandQueue(&queueDescription,IID_PPV_ARGS(queue.GetAddressOf())),"exposure queue");
        Hr(context.states->BindQueue(*queue.Get()),"exposure queue ownership");
        Hr(context.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"exposure fence");
        event.Attach(CreateEventW(nullptr,FALSE,FALSE,nullptr));Check(event.IsValid(),"exposure event");
        Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"exposure allocator");
        Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
            IID_PPV_ARGS(commands.GetAddressOf())),"exposure commands");
        Hr(commands->Close(),"exposure initial close");
        uploads.Initialize(*context.device.Get(),context.states);
        Hr(kernel.Initialize(*context.device.Get(),*context.memory,GetExecutableDirectory()/"Resources/Shaders",text),"exposure kernel");
    }
    ~Fixture(){if(!stopped){try{Wait();}catch(...){std::terminate();}}}
    void Wait()
    {
        const auto target=fenceValue;if(!target)return;
        const auto completed=fence->GetCompletedValue();Check(completed!=UINT64_MAX,"exposure device removed");
        if(completed>=target)return;
        Hr(fence->SetEventOnCompletion(target,event.Get()),"exposure completion event");
        Check(WaitForSingleObject(event.Get(),20000)==WAIT_OBJECT_0,"exposure completion timeout");
        const auto after=fence->GetCompletedValue();Check(after!=UINT64_MAX&&after>=target,"exposure completion verified");
    }
    void Begin()
    {
        Wait();Hr(allocator->Reset(),"exposure allocator reset");Hr(commands->Reset(allocator.Get(),nullptr),"exposure list reset");
    }
    void Submit(D3D12StateRecording& states)
    {
        Hr(commands->Close(),"exposure close list");
        ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);states.Commit();
        fenceValue=next;Hr(queue->Signal(fence.Get(),next++),"exposure submit signal");Wait();
    }
    ComPtr<ID3D12Resource> Readback(UINT64 bytes,const char* name)
    {
        D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;description.Width=bytes;
        description.Height=description.DepthOrArraySize=description.MipLevels=1;description.SampleDesc={1,0};
        description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> output;
        Hr(context.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(output.GetAddressOf())),name);
        Hr(context.states->Register(*output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,name),name);
        return output;
    }
    // 创建 R16G16B16A16_FLOAT 源纹理并上传；返回时资源处于 COPY_DEST，记录方自行转换。
    ComPtr<ID3D12Resource> Source(UINT width,UINT height,const std::vector<Vec3>& pixels)
    {
        Check(pixels.size()==std::size_t{width}*height,"source pixel count");
        D3D12_RESOURCE_DESC description{};
        description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;description.Width=width;description.Height=height;
        description.DepthOrArraySize=description.MipLevels=1;description.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.SampleDesc={1,0};description.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> texture;
        Hr(context.memory->Create(description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,texture,"ExposureTest.Source"),"exposure source");
        const UINT pitch=RowPitch(width,8);
        D3D12UploadSlice slice;Hr(uploads.Allocate(UINT64{pitch}*height,512,slice),"exposure source upload");
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        {
            const auto& color=pixels[std::size_t{y}*width+x];
            std::uint16_t data[4]{FloatToHalf(color.x),FloatToHalf(color.y),FloatToHalf(color.z),FloatToHalf(1.0f)};
            std::memcpy(slice.cpu+UINT64{y}*pitch+UINT64{x}*8,data,8);
        }
        Begin();D3D12StateRecording states(*context.states);
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=texture.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=slice.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset=slice.offset;
        source.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,width,height,1,pitch};
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);Submit(states);
        return texture;
    }
    // 用 1×1 R32_FLOAT 拷贝初始化曝光 ping-pong；测试适应时显式给出上一帧值。
    void SetExposure(D3D12ExposureOutput& output,float value)
    {
        D3D12UploadSlice slice;Hr(uploads.Allocate(4,512,slice),"exposure init upload");
        std::memcpy(slice.cpu,&value,4);
        Begin();D3D12StateRecording states(*context.states);
        for(auto& texture:output.exposure)
        {
            Hr(states.Use(*commands.Get(),*texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"ExposureTest.Init"),"exposure init state");
            D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=texture.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=slice.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Offset=slice.offset;
            source.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,1,1,1,256};
            commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        }
        Submit(states);
    }
    template<class T> std::vector<T> ReadBuffer(ID3D12Resource& buffer,UINT64 bytes,const char* name)
    {
        auto readback=Readback(bytes,name);
        Begin();D3D12StateRecording states(*context.states);
        Hr(states.Use(*commands.Get(),buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,"ExposureTest.BufferRead"),"buffer read state");
        Hr(states.Use(*commands.Get(),*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"ExposureTest.BufferRead"),"buffer read destination");
        commands->CopyBufferRegion(readback.Get(),0,&buffer,0,bytes);Submit(states);
        void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)},none{0,0};
        Hr(readback->Map(0,&range,&mapped),name);
        std::vector<T> result(static_cast<std::size_t>(bytes/sizeof(T)));
        std::memcpy(result.data(),mapped,static_cast<std::size_t>(bytes));readback->Unmap(0,&none);return result;
    }
    std::vector<float> ReadTexture(ID3D12Resource& texture,const char* name)
    {
        const auto description=texture.GetDesc();
        Check(description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&description.DepthOrArraySize==1,"readback is 2D");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 total=0;
        context.device->GetCopyableFootprints(&description,0,1,0,&layout,nullptr,nullptr,&total);
        auto readback=Readback(total,name);
        Begin();D3D12StateRecording states(*context.states);
        Hr(states.Use(*commands.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,"ExposureTest.TextureRead"),"texture read state");
        Hr(states.Use(*commands.Get(),*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"ExposureTest.TextureRead"),"texture read destination");
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=readback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=layout;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=&texture;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);Submit(states);
        void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(total)},none{0,0};
        Hr(readback->Map(0,&range,&mapped),name);
        std::vector<float> result(static_cast<std::size_t>(description.Width)*description.Height);
        for(UINT y=0;y<description.Height;++y)
            std::memcpy(result.data()+std::size_t{y}*description.Width,
                static_cast<const std::byte*>(mapped)+layout.Offset+UINT64{y}*layout.Footprint.RowPitch,std::size_t{description.Width}*4);
        readback->Unmap(0,&none);return result;
    }
    // 3D LUT 读回；返回值按 (r,g,b) 行优先，通道为 float3。
    std::vector<Vec3> ReadLut(ID3D12Resource& texture,const char* name)
    {
        const auto description=texture.GetDesc();
        Check(description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D,"LUT readback is 3D");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 total=0;
        UINT rows=0;UINT64 rowBytes=0;
        context.device->GetCopyableFootprints(&description,0,1,0,&layout,&rows,&rowBytes,&total);
        auto readback=Readback(total,name);
        Begin();D3D12StateRecording states(*context.states);
        Hr(states.Use(*commands.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,"ExposureTest.LutRead"),"LUT read state");
        Hr(states.Use(*commands.Get(),*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"ExposureTest.LutRead"),"LUT read destination");
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=readback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=layout;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=&texture;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);Submit(states);
        void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(total)},none{0,0};
        Hr(readback->Map(0,&range,&mapped),name);
        const UINT size=static_cast<UINT>(description.Width);
        std::vector<Vec3> result(std::size_t{size}*size*size);
        const UINT64 slicePitch=UINT64{layout.Footprint.RowPitch}*layout.Footprint.Height;
        for(UINT z=0;z<size;++z)for(UINT y=0;y<layout.Footprint.Height;++y)for(UINT x=0;x<size;++x)
        {
            const auto* data=static_cast<const std::byte*>(mapped)+layout.Offset+UINT64{z}*slicePitch
                +UINT64{y}*layout.Footprint.RowPitch+UINT64{x}*8;
            std::uint16_t bits[4]{};std::memcpy(bits,data,8);
            result[(std::size_t{x}*size+y)*size+z]=Vec3{HalfToFloat(bits[0]),HalfToFloat(bits[1]),HalfToFloat(bits[2])};
        }
        readback->Unmap(0,&none);return result;
    }
    void Shutdown()
    {
        Wait();commands.Reset();allocator.Reset();uploads.Release();kernel={};
        context.states->UnbindQueue(queue.Get());queue.Reset();fence.Reset();event.Close();
        context.states->Collect();context.memory->Trim();stopped=true;
    }
};
D3D12ExposureParameters DefaultParameters()
{
    D3D12ExposureParameters parameters;
    parameters.percentiles={-12.0f,4.0f,0.60f,0.80f};
    parameters.range={0.18f,1.0f/1024.0f,64.0f,1.0f/60.0f};
    parameters.adaptation={3.0f,1.0f,0.0f,0.0f};
    parameters.lutLift={0,0,0,float(D3D12Exposure::lutSize)};
    parameters.lutGamma={1,1,1,0};parameters.lutGain={1,1,1,0};
    return parameters;
}
ExposureSettings CpuSettings()
{
    ExposureSettings settings;
    return settings;
}
std::vector<float> Luminances(const std::vector<Vec3>& pixels)
{
    std::vector<float> output;output.reserve(pixels.size());
    for(const auto& color:pixels)output.push_back(Luminance(color));
    return output;
}
void DumpMessages(D3D12Context& context)
{
    ComPtr<ID3D12InfoQueue> queue;if(FAILED(context.device.As(&queue)))return;
    const auto count=queue->GetNumStoredMessages();
    for(UINT64 index=0;index<count;++index)
    {
        SIZE_T size=0;if(FAILED(queue->GetMessage(index,nullptr,&size)))continue;
        std::vector<std::byte> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if(FAILED(queue->GetMessage(index,message,&size)))continue;
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)
            std::cerr<<"D3D12["<<int(message->Severity)<<"] "<<message->pDescription<<'\n';
    }
}
void Uniform(Fixture& fixture,nlohmann::json& evidence)
{
    const UINT width=64,height=64;
    const std::vector<Vec3> pixels(std::size_t{width}*height,Vec3{0.18f,0.18f,0.18f});
    auto source=fixture.Source(width,height,pixels);
    D3D12ExposureOutput output;Hr(fixture.kernel.CreateOutput(*source.Get(),width,height,output),"uniform output");
    fixture.Begin();D3D12StateRecording states(*fixture.context.states);
    // LUT 使用非平凡 lift/gamma/gain；曝光参数保持默认。
    ColorGradingSettings grading;grading.lift[0]=0.02f;grading.lift[1]=0.01f;grading.lift[2]=0.0f;
    grading.gamma[0]=1.2f;grading.gamma[1]=1.0f;grading.gamma[2]=0.8f;
    grading.gain[0]=1.1f;grading.gain[1]=0.9f;grading.gain[2]=1.0f;
    auto parameters=DefaultParameters();
    parameters.lutLift={grading.lift[0],grading.lift[1],grading.lift[2],float(D3D12Exposure::lutSize)};
    parameters.lutGamma={grading.gamma[0],grading.gamma[1],grading.gamma[2],0};
    parameters.lutGain={grading.gain[0],grading.gain[1],grading.gain[2],0};
    Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output,parameters,true,true,true),"uniform record");
    fixture.Submit(states);
    const auto written=output.exposureIndex;
    auto histogram=fixture.ReadBuffer<std::uint32_t>(*output.histogram.Get(),D3D12Exposure::histogramBins*4,"exposure histogram");
    const auto exposure=fixture.ReadTexture(*output.exposure[written].Get(),"exposure value");
    const auto lut=fixture.ReadLut(*output.lut.Get(),"exposure lut");
    // 读回命令会改变状态；提交由 Read* 内部完成，Record 已提交。
    const auto cpu=BuildExposureHistogram(Luminances(pixels),CpuSettings());
    std::uint64_t gpuPixels=0;
    for(UINT bin=0;bin<D3D12Exposure::histogramBins;++bin){Check(histogram[bin]==cpu.bins[bin],"uniform histogram bin matches CPU");gpuPixels+=histogram[bin];}
    Check(gpuPixels==std::size_t{width}*height,"uniform histogram counts every pixel");
    AutoExposure cpuExposure;cpuExposure.Reset(1.0f);float expected=0;
    Check(cpuExposure.Update(cpu,1.0f/60.0f,CpuSettings(),expected),"uniform CPU exposure");
    Check(std::abs(exposure[0]-expected)<std::max(1e-4f,expected*2e-3f),"uniform GPU exposure matches CPU target");
    // LUT：非平凡 LGG 下逐个晶格点对照 ColorLut::Sample。
    ColorLut colorLut;Check(colorLut.Build(D3D12Exposure::lutSize,grading),"CPU LUT build");
    float maximumLutError=0;const UINT size=D3D12Exposure::lutSize;
    for(UINT r=0;r<size;++r)for(UINT g=0;g<size;++g)for(UINT b=0;b<size;++b)
    {
        const Vec3 coordinate{float(r)/float(size-1),float(g)/float(size-1),float(b)/float(size-1)};
        const auto wanted=colorLut.Sample(coordinate);
        const auto& actual=lut[(std::size_t{r}*size+g)*size+b];
        maximumLutError=std::max(maximumLutError,std::max({std::abs(actual.x-wanted.x),std::abs(actual.y-wanted.y),std::abs(actual.z-wanted.z)}));
    }
    Check(maximumLutError<1.5e-3f,"GPU LUT matches CPU lattice");
    evidence["uniform"]={{"pixels",gpuPixels},{"exposure",exposure[0]},{"cpu_exposure",expected},{"lut_max_error",maximumLutError}};
    std::cout<<"Exposure uniform: pixels="<<gpuPixels<<", exposure="<<exposure[0]<<", LUT max error="<<maximumLutError<<'\n';
}
void AssetLut(Fixture& fixture,nlohmann::json& evidence,const fs::path& output)
{
    // G15：`.selut` 资产 → 33³ LUT 上传。先按文件加载，再逐晶格对照 CPU；
    // 改写文件并轮询后再次上传，确认 revision 变化会替换 GPU 数据。
    const UINT width=32,height=32;
    auto source=fixture.Source(width,height,std::vector<Vec3>(std::size_t{width}*height,Vec3{0.25f,0.25f,0.25f}));
    D3D12ExposureOutput exposure;Hr(fixture.kernel.CreateOutput(*source.Get(),width,height,exposure),"asset LUT output");
    ColorGradingSettings grading;grading.lift[0]=0.03f;grading.lift[1]=0.0f;grading.lift[2]=0.01f;
    grading.gamma[0]=0.85f;grading.gamma[1]=1.15f;grading.gamma[2]=1.0f;
    grading.gain[0]=1.2f;grading.gain[1]=0.9f;grading.gain[2]=1.05f;
    ColorLut authored;Check(authored.Build(D3D12Exposure::lutSize,grading),"asset LUT builds");
    const auto path=output/"exposure-asset-lut.selut";
    std::string error;
    Check(authored.SaveAsset(path,ColorLutColorSpace::Linear,error),"asset LUT writes");
    ColorLutFile file(0.0f);
    Check(file.Open(path,error)&&file.Ready()&&file.Status().revision==1,"asset LUT loads from disk");
    const auto maximumError=[&](const std::vector<Vec3>& gpu,const ColorLut& cpu)
    {
        float maximum=0;const UINT size=D3D12Exposure::lutSize;
        for(UINT r=0;r<size;++r)for(UINT g=0;g<size;++g)for(UINT b=0;b<size;++b)
        {
            const Vec3 coordinate{float(r)/float(size-1),float(g)/float(size-1),float(b)/float(size-1)};
            const auto wanted=cpu.Sample(coordinate);
            const auto& actual=gpu[(std::size_t{r}*size+g)*size+b];
            maximum=std::max(maximum,std::max({std::abs(actual.x-wanted.x),std::abs(actual.y-wanted.y),std::abs(actual.z-wanted.z)}));
        }
        return maximum;
    };
    fixture.Begin();D3D12StateRecording states(*fixture.context.states);
    Hr(fixture.kernel.UploadLut(*fixture.commands.Get(),states,exposure,*file.Values()),"asset LUT upload");
    fixture.Submit(states);
    const auto lutState=fixture.context.states->Find(*exposure.lut.Get());
    Check(lutState&&lutState->states[0]==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"uploaded LUT ends sampleable");
    auto uploaded=fixture.ReadLut(*exposure.lut.Get(),"asset lut");
    const auto firstError=maximumError(uploaded,file.Lut());
    Check(firstError<1.5e-3f,"uploaded asset LUT matches CPU lattice");
    // 契约：非 33³ 或非有限值在记录前拒绝，不改变状态。
    std::vector<Vec3> wrongSize(32u*32u*32u,Vec3{0,0,0});
    D3D12StateRecording rejected(*fixture.context.states);
    const auto pending=rejected.PendingTransitions();
    Check(fixture.kernel.UploadLut(*fixture.commands.Get(),rejected,exposure,wrongSize)==E_INVALIDARG
        &&rejected.PendingTransitions()==pending,"asset LUT rejects a non-33 lattice");
    auto nonFinite=*file.Values();nonFinite[0].x=std::numeric_limits<float>::quiet_NaN();
    Check(fixture.kernel.UploadLut(*fixture.commands.Get(),rejected,exposure,nonFinite)==E_INVALIDARG
        &&rejected.PendingTransitions()==pending,"asset LUT rejects non-finite lattice values");
    // 热重载：改写文件 → Poll 增加 revision → 再上传，GPU 数据随之替换。
    ColorGradingSettings second;second.gain[0]=0.7f;second.gain[1]=1.0f;second.gain[2]=1.3f;
    ColorLut secondLut;Check(secondLut.Build(D3D12Exposure::lutSize,second),"second asset LUT builds");
    Check(secondLut.SaveAsset(path,ColorLutColorSpace::Linear,error),"second asset LUT writes");
    std::error_code ignored;
    const auto stamp=std::filesystem::last_write_time(path,ignored);
    if(!ignored)std::filesystem::last_write_time(path,stamp+std::chrono::seconds(1),ignored);
    Check(file.Poll()&&file.Status().revision==2,"asset LUT hot reloads");
    fixture.Begin();D3D12StateRecording reloadStates(*fixture.context.states);
    Hr(fixture.kernel.UploadLut(*fixture.commands.Get(),reloadStates,exposure,*file.Values()),"hot-reloaded asset LUT upload");
    fixture.Submit(reloadStates);
    uploaded=fixture.ReadLut(*exposure.lut.Get(),"hot-reloaded asset lut");
    const auto secondError=maximumError(uploaded,secondLut);
    Check(secondError<1.5e-3f,"hot-reloaded asset LUT replaces the GPU lattice");
    const auto firstRed=secondLut.Sample({0.75f,0.25f,0.5f}),firstRedOld=authored.Sample({0.75f,0.25f,0.5f});
    Check(std::abs(firstRed.x-firstRedOld.x)>1e-3f,"hot reload changes graded values");
    evidence["asset_lut"]={{"revision",file.Status().revision},{"size",file.Status().size},
        {"first_max_error",firstError},{"second_max_error",secondError}};
    std::cout<<"Exposure asset LUT: first max error="<<firstError
        <<", hot-reloaded max error="<<secondError<<" (revision 2)\n";
}
void MixedAndNonFinite(Fixture& fixture,nlohmann::json& evidence)
{
    const UINT width=96,height=64;
    std::vector<Vec3> pixels(std::size_t{width}*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
    {
        if(x<width/2)pixels[std::size_t{y}*width+x]={0.05f,0.05f,0.05f};
        else pixels[std::size_t{y}*width+x]={0.5f,0.5f,0.5f};
    }
    // 12 个非有限像素必须被跳过：R=NaN、G=+Inf、B=-Inf 各 4 个。
    for(UINT index=0;index<12;++index)
    {
        auto& color=pixels[index];
        if(index%3==0)color.x=std::numeric_limits<float>::quiet_NaN();
        else if(index%3==1)color.y=std::numeric_limits<float>::infinity();
        else color.z=-std::numeric_limits<float>::infinity();
    }
    auto source=fixture.Source(width,height,pixels);
    D3D12ExposureOutput output;Hr(fixture.kernel.CreateOutput(*source.Get(),width,height,output),"mixed output");
    fixture.Begin();D3D12StateRecording states(*fixture.context.states);
    Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output,DefaultParameters(),true,true,true),"mixed record");
    fixture.Submit(states);
    const auto written=output.exposureIndex;
    auto histogram=fixture.ReadBuffer<std::uint32_t>(*output.histogram.Get(),D3D12Exposure::histogramBins*4,"mixed histogram");
    const auto exposure=fixture.ReadTexture(*output.exposure[written].Get(),"mixed exposure");
    auto luminances=Luminances(pixels);
    std::vector<float> finite;
    for(std::size_t index=0;index<pixels.size();++index)
    {
        const auto& color=pixels[index];
        // GPU 约定：任一侧通道非有限即跳过整像素，与 CPU oracle 的推导保持一致。
        if(std::isfinite(color.x)&&std::isfinite(color.y)&&std::isfinite(color.z))finite.push_back(luminances[index]);
    }
    const auto cpu=BuildExposureHistogram(finite,CpuSettings());
    for(UINT bin=0;bin<D3D12Exposure::histogramBins;++bin)Check(histogram[bin]==cpu.bins[bin],"mixed histogram skips non-finite pixels");
    Check(cpu.pixels==std::size_t{width}*height-12,"CPU oracle also skips non-finite pixels");
    AutoExposure cpuExposure;cpuExposure.Reset(1.0f);float expected=0;
    cpuExposure.Update(cpu,1.0f/60.0f,CpuSettings(),expected);
    Check(std::abs(exposure[0]-expected)<std::max(1e-4f,expected*2e-3f),"mixed GPU exposure matches CPU percentiles");
    evidence["mixed"]={{"pixels",cpu.pixels},{"exposure",exposure[0]},{"cpu_exposure",expected}};
    std::cout<<"Exposure mixed: pixels="<<cpu.pixels<<"/(96*64), exposure="<<exposure[0]<<'\n';
}
void AdaptationStep(Fixture& fixture,nlohmann::json& evidence)
{
    const UINT width=16,height=16;
    const std::vector<Vec3> neutral(std::size_t{width}*height,Vec3{0.18f,0.18f,0.18f});
    const std::vector<Vec3> dark(std::size_t{width}*height,Vec3{0.09f,0.09f,0.09f});
    auto source=fixture.Source(width,height,dark);
    D3D12ExposureOutput output;Hr(fixture.kernel.CreateOutput(*source.Get(),width,height,output),"adaptation output");
    fixture.SetExposure(output,1.0f);
    const auto parameters=DefaultParameters();
    fixture.Begin();D3D12StateRecording states(*fixture.context.states);
    for(int frame=0;frame<60;++frame)
        Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output,parameters,true,false,frame==0),"adaptation record");
    fixture.Submit(states);
    const auto written=output.exposureIndex;
    const auto exposure=fixture.ReadTexture(*output.exposure[written].Get(),"adaptation exposure");
    const auto histogram=fixture.ReadBuffer<std::uint32_t>(*output.histogram.Get(),D3D12Exposure::histogramBins*4,"adaptation histogram");
    UINT64 histogramPixels=0;UINT firstNonZero=0;for(UINT i=0;i<D3D12Exposure::histogramBins;++i){histogramPixels+=histogram[i];if(histogram[i]&&!firstNonZero)firstNonZero=i;}
    Check(histogramPixels==256&&firstNonZero>0,"adaptation frames rebuild the histogram from the dark source");
    const auto cpuHistogram=BuildExposureHistogram(Luminances(dark),CpuSettings());
    AutoExposure cpuExposure;cpuExposure.Reset(1.0f);float expected=0;
    cpuExposure.Update(BuildExposureHistogram(Luminances(neutral),CpuSettings()),1.0f/60.0f,CpuSettings(),expected);
    for(int frame=0;frame<60;++frame)cpuExposure.Update(cpuHistogram,1.0f/60.0f,CpuSettings(),expected);
    std::cout<<"  adaptation GPU="<<exposure[0]<<" CPU="<<expected<<'\n';
    Check(std::abs(exposure[0]-expected)<2e-3f,"60-frame dark step matches CPU exponential adaptation");    Check(exposure[0]>1.85f&&exposure[0]<2.05f,"dark-to-bright step approaches 2x within one second");
    evidence["adaptation"]={{"gpu",exposure[0]},{"cpu",expected},{"frames",60}};
    std::cout<<"Exposure adaptation: GPU="<<exposure[0]<<" CPU="<<expected<<'\n';
}
void ClampAndContracts(Fixture& fixture,nlohmann::json& evidence)
{
    const UINT width=32,height=32;
    const auto before=fixture.context.states->Stats().resources;
    {
        const std::vector<Vec3> black(std::size_t{width}*height,Vec3{0.0f,0.0f,0.0f});
        auto source=fixture.Source(width,height,black);
        D3D12ExposureOutput output;Hr(fixture.kernel.CreateOutput(*source.Get(),width,height,output),"black output");
        fixture.Begin();D3D12StateRecording states(*fixture.context.states);
        Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output,DefaultParameters(),true,true,true),"black record");
        fixture.Submit(states);
        const auto exposure=fixture.ReadTexture(*output.exposure[output.exposureIndex].Get(),"black exposure");
        Check(std::isfinite(exposure[0])&&exposure[0]==DefaultParameters().range[2],"all-black clamps to maximum exposure");
        const auto histogram=fixture.ReadBuffer<std::uint32_t>(*output.histogram.Get(),D3D12Exposure::histogramBins*4,"black histogram");
        Check(histogram[0]==width*height,"all-black pixels enter the lowest bucket");
        evidence["clamp"]={{"black_exposure",exposure[0]}};
    }
    {
        const std::vector<Vec3> white(std::size_t{width}*height,Vec3{60000.0f,60000.0f,60000.0f});
        auto source=fixture.Source(width,height,white);
        D3D12ExposureOutput output;Hr(fixture.kernel.CreateOutput(*source.Get(),width,height,output),"white output");
        fixture.Begin();D3D12StateRecording states(*fixture.context.states);
        Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output,DefaultParameters(),true,true,false),"white record");
        fixture.Submit(states);
        const auto exposure=fixture.ReadTexture(*output.exposure[output.exposureIndex].Get(),"white exposure");
        const auto cpuHistogram=BuildExposureHistogram(Luminances(white),CpuSettings());
        AutoExposure cpuExposure;cpuExposure.Reset(1.0f);float expected=0;
        cpuExposure.Update(cpuHistogram,1.0f/60.0f,CpuSettings(),expected);
        Check(std::abs(exposure[0]-expected)<std::max(1e-4f,expected*2e-3f),"all-white exposure matches CPU clamp");
        Check(exposure[0]>=DefaultParameters().range[1]&&exposure[0]<1.0f,"all-white lowers exposure without leaving the range");
        evidence["clamp"]["white_exposure"]=exposure[0];
    }
    D3D12Exposure uninitialized;D3D12ExposureOutput empty;
    auto source=fixture.Source(8,8,std::vector<Vec3>(64,Vec3{0.2f,0.2f,0.2f}));
    Check(uninitialized.CreateOutput(*source.Get(),8,8,empty)==E_UNEXPECTED,"uninitialized exposure kernel rejected");
    Check(fixture.kernel.CreateOutput(*source.Get(),4,4,empty)==E_INVALIDARG,"mismatched exposure source size rejected");
    Check(!empty.histogram&&!empty.exposure[0],"failed output creation leaves output empty");
    Hr(fixture.kernel.CreateOutput(*source.Get(),8,8,empty),"valid exposure output");
    const auto* old=empty.histogram.Get();
    Check(fixture.kernel.CreateOutput(*source.Get(),4,4,empty)==E_INVALIDARG&&empty.histogram.Get()==old,"failed replacement preserves output");
    fixture.Begin();
    D3D12StateRecording states(*fixture.context.states);
    D3D12ExposureOutput blank;
    Check(fixture.kernel.Record(*fixture.commands.Get(),states,blank,DefaultParameters(),true,true,true)==E_INVALIDARG
        &&states.PendingTransitions()==0,"empty exposure record rejected before transitions");
    Check(fixture.kernel.Record(*fixture.commands.Get(),states,empty,DefaultParameters(),false,false,false)==S_OK
        &&states.PendingTransitions()==0,"disabled record without LUT rebuild is a no-op");
    Hr(fixture.kernel.Record(*fixture.commands.Get(),states,empty,DefaultParameters(),true,true,true),"valid exposure record");
    Check(states.PendingTransitions()>0&&states.PendingUavBarriers()>0,"valid exposure record emits transitions and UAV barriers");
    fixture.Submit(states);
    const auto exposureState=fixture.context.states->Find(*empty.exposure[empty.exposureIndex].Get());
    const auto histogramState=fixture.context.states->Find(*empty.histogram.Get());
    const auto lutState=fixture.context.states->Find(*empty.lut.Get());
    Check(exposureState&&exposureState->states[0]==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"exposure ends sampleable");
    Check(histogramState&&histogramState->states[0]==D3D12_RESOURCE_STATE_COMMON,"histogram buffer follows the engine buffer decay to common");
    Check(lutState&&lutState->states[0]==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"LUT ends sampleable");
    const auto after=fixture.context.states->Stats().resources;
    Check(after>=before,"state registry tracks exposure resources");
    evidence["contracts"]={{"resources",after},{"exposure_index",empty.exposureIndex},{"frames",empty.frames}};
    std::cout<<"Exposure contracts: resources="<<after<<", exposure_index="<<empty.exposureIndex<<", frames="<<empty.frames<<'\n';
}
struct StepSample
{
    int frame=0;
    double seconds=0,gpu=0,cpu=0,target=0;
};
std::vector<StepSample> RunStep(Fixture& fixture,D3D12ExposureOutput& output,const ExposureHistogram& previous,
    const ExposureHistogram& next,float start,int frames)
{
    fixture.SetExposure(output,start);
    AutoExposure cpu;cpu.Reset(start);float cpuValue=start;
    // 上一场景的直方图只用于把 CPU 参考标记为“已初始化”，实际值仍等于 start。
    cpu.Update(previous,0.0f,CpuSettings(),cpuValue);
    Check(std::abs(cpuValue-start)<1e-4f,"step start aligns CPU and GPU history");
    std::vector<StepSample> samples;samples.reserve(frames);
    for(int frame=0;frame<frames;++frame)
    {
        fixture.Begin();D3D12StateRecording states(*fixture.context.states);
        Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output,DefaultParameters(),true,false,false),"step record");
        fixture.Submit(states);
        const auto measured=fixture.ReadTexture(*output.exposure[output.exposureIndex].Get(),"step exposure");
        Check(measured.size()==1&&std::isfinite(measured[0]),"step exposure readback is finite");
        float expected=0;cpu.Update(next,1.0f/60.0f,CpuSettings(),expected);
        samples.push_back({frame,double(frame+1)/60.0,measured[0],expected,cpu.Target()});
    }
    return samples;
}
void AppendSvgPlot(std::ofstream& file,const std::vector<StepSample>& samples,double left,double top,
    double width,double height,const char* title,const char* color)
{
    float minimum=float(samples.front().gpu),maximum=minimum;
    for(const auto& sample:samples)
    {
        minimum=std::min({minimum,float(sample.gpu),float(sample.cpu),float(sample.target)});
        maximum=std::max({maximum,float(sample.gpu),float(sample.cpu),float(sample.target)});
    }
    const auto padding=std::max(0.01f,(maximum-minimum)*0.08f);
    const auto low=double(minimum)-padding,high=double(maximum)+padding;
    const auto span=high-low>1e-6?high-low:1.0;
    const auto x=[&](int frame){return left+width*double(frame)/double(std::max(1,int(samples.size())-1));};
    const auto y=[&](double value){return top+height*(1.0-(value-low)/span);};
    file<<std::fixed<<std::setprecision(2);
    file<<"<g><text x=\""<<left<<"\" y=\""<<top-12<<"\" font-size=\"14\" fill=\"#222\">"<<title<<"</text>\n";
    file<<"<rect x=\""<<left<<"\" y=\""<<top<<"\" width=\""<<width<<"\" height=\""<<height
        <<"\" fill=\"#fbfbfb\" stroke=\"#777\"/>\n";
    for(int step=0;step<=3;++step)
    {
        const auto value=low+span*step/3.0,py=y(value);
        file<<"<line x1=\""<<left<<"\" y1=\""<<py<<"\" x2=\""<<left+width<<"\" y2=\""<<py
            <<"\" stroke=\"#e4e4e4\"/>\n";
        file<<"<text x=\""<<left-8<<"\" y=\""<<py+4<<"\" font-size=\"10\" text-anchor=\"end\" fill=\"#555\">"
            <<value<<"</text>\n";
    }
    const auto polyline=[&](bool gpu,const char* stroke,const char* dash)
    {
        file<<"<polyline fill=\"none\" stroke=\""<<stroke<<"\" stroke-width=\"2\"";
        if(dash)file<<" stroke-dasharray=\""<<dash<<"\"";
        file<<" points=\"";
        file<<std::setprecision(2);
        for(const auto& sample:samples)
            file<<x(sample.frame)<<","<<y(gpu?sample.gpu:sample.cpu)<<" ";
        file<<"\"/>\n";
    };
    polyline(true,color,nullptr);polyline(false,"#c44e52","5 4");
    file<<std::setprecision(2);
    file<<"<text x=\""<<left+width-4<<"\" y=\""<<top+14<<"\" font-size=\"11\" text-anchor=\"end\" fill=\"#555\">"
        <<samples.front().seconds<<".."<<samples.back().seconds<<" s</text>\n";
    file<<"</g>\n";
}
void StepCurve(Fixture& fixture,nlohmann::json& evidence,const fs::path& output)
{
    const UINT width=16,height=16;
    const std::vector<Vec3> dark(std::size_t{width}*height,Vec3{0.09f,0.09f,0.09f});
    const std::vector<Vec3> bright(std::size_t{width}*height,Vec3{0.36f,0.36f,0.36f});
    const auto darkHistogram=BuildExposureHistogram(Luminances(dark),CpuSettings());
    const auto brightHistogram=BuildExposureHistogram(Luminances(bright),CpuSettings());
    const auto darkTarget=[&]
    {
        AutoExposure cpu;cpu.Reset(1.0f);float value=0;
        cpu.Update(darkHistogram,0.0f,CpuSettings(),value);return value;
    }();
    const auto brightTarget=[&]
    {
        AutoExposure cpu;cpu.Reset(1.0f);float value=0;
        cpu.Update(brightHistogram,0.0f,CpuSettings(),value);return value;
    }();
    Check(std::abs(darkTarget-2.0f)<0.05f&&std::abs(brightTarget-0.5f)<0.02f,"uniform step targets match key/luminance");
    auto darkSource=fixture.Source(width,height,dark);
    auto brightSource=fixture.Source(width,height,bright);
    D3D12ExposureOutput darkSeed,brightSeed,brightStep,darkStep;
    Hr(fixture.kernel.CreateOutput(*darkSource.Get(),width,height,darkSeed),"step dark seed output");
    Hr(fixture.kernel.CreateOutput(*brightSource.Get(),width,height,brightSeed),"step bright seed output");
    Hr(fixture.kernel.CreateOutput(*brightSource.Get(),width,height,brightStep),"step bright output");
    Hr(fixture.kernel.CreateOutput(*darkSource.Get(),width,height,darkStep),"step dark output");
    const auto seed=[&](D3D12ExposureOutput& target,const char* name)
    {
        fixture.SetExposure(target,1.0f);
        fixture.Begin();D3D12StateRecording states(*fixture.context.states);
        Hr(fixture.kernel.Record(*fixture.commands.Get(),states,target,DefaultParameters(),true,true,false),name);
        fixture.Submit(states);
        const auto value=fixture.ReadTexture(*target.exposure[target.exposureIndex].Get(),name);
        Check(value.size()==1&&std::isfinite(value[0]),"step seed exposure readback");
        return value[0];
    };
    const auto darkSeedValue=seed(darkSeed,"step dark seed");
    const auto brightSeedValue=seed(brightSeed,"step bright seed");
    Check(std::abs(darkSeedValue-darkTarget)<2e-3f&&std::abs(brightSeedValue-brightTarget)<2e-3f,
        "GPU seed exposure matches CPU target");
    // 每段 2 s（120 帧）：暗场景稳定在 2.0 后切到亮场景，曝光应按 3/s 下降；
    // 反向切回时按 1/s 上升。曲线逐帧读回 GPU 1×1 曝光纹理并与 CPU 公式对照。
    const auto brightSamples=RunStep(fixture,brightStep,darkHistogram,brightHistogram,darkTarget,120);
    const auto darkSamples=RunStep(fixture,darkStep,brightHistogram,darkHistogram,brightTarget,120);
    double gpuCpuError=0;
    for(const auto& sample:brightSamples)gpuCpuError=std::max(gpuCpuError,std::abs(sample.gpu-sample.cpu));
    for(const auto& sample:darkSamples)gpuCpuError=std::max(gpuCpuError,std::abs(sample.gpu-sample.cpu));
    Check(gpuCpuError<2e-3,"step curve GPU exposure matches CPU adaptation every frame");
    bool brightMonotonic=true,darkMonotonic=true;
    for(std::size_t index=1;index<brightSamples.size();++index)
    {
        brightMonotonic&=brightSamples[index].gpu<=brightSamples[index-1].gpu+1e-6;
        darkMonotonic&=darkSamples[index].gpu>=darkSamples[index-1].gpu-1e-6;
    }
    Check(brightMonotonic&&darkMonotonic,"step curves are monotonic toward the new target");
    const auto brightOneSecond=brightSamples[59],darkOneSecond=darkSamples[59];
    // 适应速度按曝光方向选择：曝光下降（场景变亮）走 darkAdaptation=1/s，
    // 曝光上升（场景变暗）走 brightAdaptation=3/s；与 AutoExposure::Update 一致。
    const auto expectedFallOneSecond=double(brightTarget)+(double(darkTarget)-brightTarget)*std::exp(-1.0);
    const auto expectedRiseOneSecond=double(darkTarget)+(double(brightTarget)-darkTarget)*std::exp(-3.0);
    Check(std::abs(brightOneSecond.cpu-expectedFallOneSecond)<1e-4,"falling exposure follows 1/s after one second");
    Check(std::abs(darkOneSecond.cpu-expectedRiseOneSecond)<1e-4,"rising exposure follows 3/s after one second");
    Check(std::abs(brightSamples.back().gpu-brightTarget)<0.25,"dark scene uses the slower one-second time constant");
    Check(std::abs(darkSamples.back().gpu-darkTarget)<0.03,"bright scene converges toward its target in two seconds");
    std::ofstream csv(output/"step-curve.csv");
    csv<<"phase,frame,seconds,gpu_exposure,cpu_exposure,cpu_target\n";
    const auto write=[&](const char* phase,const std::vector<StepSample>& samples)
    {
        for(const auto& sample:samples)
            csv<<phase<<','<<sample.frame<<','<<std::fixed<<std::setprecision(6)<<sample.seconds<<','
                <<sample.gpu<<','<<sample.cpu<<','<<sample.target<<'\n';
    };
    write("dark_to_bright",brightSamples);write("bright_to_dark",darkSamples);
    Check(bool(csv),"write step curve CSV");
    std::ofstream svg(output/"step-curve.svg");
    svg<<"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"920\" height=\"560\" viewBox=\"0 0 920 560\">\n"
        <<"<rect width=\"920\" height=\"560\" fill=\"#ffffff\"/>\n"
        <<"<text x=\"30\" y=\"28\" font-size=\"17\" font-family=\"sans-serif\" fill=\"#222\">G15 GPU auto exposure step response</text>\n"
        <<"<text x=\"30\" y=\"48\" font-size=\"11\" font-family=\"sans-serif\" fill=\"#666\">solid = GPU 1x1 exposure texture, dashed = CPU AutoExposure; 60 Hz frames</text>\n";
    AppendSvgPlot(svg,brightSamples,70,90,800,190,"scene dark 0.09 -> bright 0.36 (falling exposure, 1/s)","#1e6fd9");
    AppendSvgPlot(svg,darkSamples,70,340,800,190,"scene bright 0.36 -> dark 0.09 (rising exposure, 3/s)","#e08a1e");
    svg<<"</svg>\n";Check(bool(svg),"write step curve SVG");
    evidence["step_curve"]={{"frames",120},{"dark_to_bright_start",darkTarget},{"dark_to_bright_final",brightSamples.back().gpu},
        {"bright_to_dark_start",brightTarget},{"bright_to_dark_final",darkSamples.back().gpu},
        {"gpu_cpu_max_error",gpuCpuError},{"bright_one_second",brightOneSecond.cpu},{"dark_one_second",darkOneSecond.cpu}};
    std::cout<<"Exposure step: dark->bright 1 s="<<brightOneSecond.gpu<<" 2 s="<<brightSamples.back().gpu
        <<", bright->dark 1 s="<<darkOneSecond.gpu<<" 2 s="<<darkSamples.back().gpu
        <<", GPU-CPU max error="<<gpuCpuError<<'\n';
}
void Cost(Fixture& fixture,nlohmann::json& evidence)
{    const UINT width=1920,height=1080;
    const std::vector<Vec3> pixels(std::size_t{width}*height,Vec3{0.2f,0.3f,0.4f});
    auto source=fixture.Source(width,height,pixels);
    D3D12ExposureOutput output;Hr(fixture.kernel.CreateOutput(*source.Get(),width,height,output),"cost output");
    D3D12_QUERY_HEAP_DESC queryDescription{};queryDescription.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;queryDescription.Count=2;
    ComPtr<ID3D12QueryHeap> queries;Hr(fixture.context.device->CreateQueryHeap(&queryDescription,IID_PPV_ARGS(queries.GetAddressOf())),"cost queries");
    ComPtr<ID3D12Resource> readback=fixture.Readback(16,"cost timestamps");
    UINT64 frequency=0;Hr(fixture.queue->GetTimestampFrequency(&frequency),"cost frequency");
    fixture.Begin();D3D12StateRecording states(*fixture.context.states);
    fixture.commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
    for(int iteration=0;iteration<32;++iteration)
        Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output,DefaultParameters(),true,iteration==0,iteration==0),"cost record");
    fixture.commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
    Hr(states.Use(*fixture.commands.Get(),*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"cost readback"),"cost readback state");
    fixture.commands->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,readback.Get(),0);
    fixture.Submit(states);
    void* mapped=nullptr;D3D12_RANGE range{0,16},none{0,0};Hr(readback->Map(0,&range,&mapped),"cost map");
    UINT64 timestamps[2]{};std::memcpy(timestamps,mapped,16);readback->Unmap(0,&none);
    const double milliseconds=double(timestamps[1]-timestamps[0])*1000.0/double(frequency)/32.0;
    Check(milliseconds>0&&milliseconds<5.0,"1080p histogram+adapt stays within a loose cost bound");
    evidence["cost"]={{"width",width},{"height",height},{"milliseconds",milliseconds}};
    std::cout<<"Exposure cost: 1920x1080 histogram+adapt "<<milliseconds<<" ms/frame\n";
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply exposure output directory");
        const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"exposure localization");
        Fixture fixture(text);
        nlohmann::json evidence;
        Uniform(fixture,evidence);
        AssetLut(fixture,evidence,output);
        MixedAndNonFinite(fixture,evidence);
        AdaptationStep(fixture,evidence);
        StepCurve(fixture,evidence,output);
        ClampAndContracts(fixture,evidence);
        Cost(fixture,evidence);
        DumpMessages(fixture.context);
        fixture.Shutdown();
        Check(fixture.context.states->Stats().resources==0&&fixture.context.memory->Stats().used==0
            &&fixture.context.memory->Stats().reserved==0,"all exposure allocations released");
        evidence["gpu"]={{"state_resources_after",fixture.context.states->Stats().resources},
            {"memory_used_after",fixture.context.memory->Stats().used}};
        std::ofstream report(output/"validation.json");report<<evidence.dump(2)<<'\n';
        Check(bool(report),"exposure evidence output");
        std::cout<<"GpuExposureTests passed (GPU histogram, 60-frame adaptation, clamp, NaN/Inf skip, 33^3 LUT, 1080p cost).\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<"GpuExposureTests: "<<error.what()<<'\n';return 1;}
}
