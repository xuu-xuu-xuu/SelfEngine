// G14：GPU TAA 解析对照 CPU ResolveTaa。覆盖静止混合、深度/速度 disocclusion、
// 历史失效、YCoCg 邻域夹取、历史深度写回、ping-pong 与非法输入。
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Graphics/D3D12/D3D12StateTracker.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Taa.h"
#include "PlatformLayer/Graphics/D3D12/D3D12UploadArena.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "FunctionLayer/Rendering/TemporalAA.h"
#include "CoreLayer/Math/Half.h"
#include "CoreLayer/Localization.h"
#include <d3d12sdklayers.h>
#include <nlohmann/json.hpp>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace se;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
UINT RowPitch(UINT width,UINT bytesPerPixel){return (width*bytesPerPixel+255)&~255u;}
Vec3 HalfColor(Vec3 value)
{
    return {HalfToFloat(FloatToHalf(value.x)),HalfToFloat(FloatToHalf(value.y)),HalfToFloat(FloatToHalf(value.z))};
}
float HalfValue(float value){return HalfToFloat(FloatToHalf(value));}
struct Fixture
{
    D3D12Context context;
    D3D12Taa kernel;
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
        Check(InitializeD3D12Context(context,text),"TAA device");
        D3D12_COMMAND_QUEUE_DESC queueDescription{};queueDescription.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(context.device->CreateCommandQueue(&queueDescription,IID_PPV_ARGS(queue.GetAddressOf())),"TAA queue");
        Hr(context.states->BindQueue(*queue.Get()),"TAA queue ownership");
        Hr(context.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"TAA fence");
        event.Attach(CreateEventW(nullptr,FALSE,FALSE,nullptr));Check(event.IsValid(),"TAA event");
        Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"TAA allocator");
        Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
            IID_PPV_ARGS(commands.GetAddressOf())),"TAA commands");
        Hr(commands->Close(),"TAA initial close");
        uploads.Initialize(*context.device.Get(),context.states);
        Hr(kernel.Initialize(*context.device.Get(),*context.memory,GetExecutableDirectory()/"Resources/Shaders",text),"TAA kernel");
    }
    ~Fixture(){if(!stopped){try{Wait();}catch(...){std::terminate();}}}
    void Wait()
    {
        const auto target=fenceValue;if(!target)return;
        const auto completed=fence->GetCompletedValue();Check(completed!=UINT64_MAX,"TAA device removed");
        if(completed>=target)return;
        Hr(fence->SetEventOnCompletion(target,event.Get()),"TAA completion event");
        Check(WaitForSingleObject(event.Get(),20000)==WAIT_OBJECT_0,"TAA completion timeout");
    }
    void Begin(){Wait();Hr(allocator->Reset(),"TAA allocator reset");Hr(commands->Reset(allocator.Get(),nullptr),"TAA list reset");}
    void Submit(D3D12StateRecording& states)
    {
        Hr(commands->Close(),"TAA close list");
        ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);states.Commit();
        fenceValue=next;Hr(queue->Signal(fence.Get(),next++),"TAA submit signal");Wait();
    }
    // 把 source 复制到 destination；两者状态由记录器转换，避免依赖创建时状态。
    void CopyTexture(ID3D12Resource& destination,ID3D12Resource& source)
    {
        Begin();D3D12StateRecording states(*context.states);
        Hr(states.Use(*commands.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,"TaaTest.CopySource"),"copy source");
        Hr(states.Use(*commands.Get(),destination,D3D12_RESOURCE_STATE_COPY_DEST,"TaaTest.CopyDestination"),"copy destination");
        commands->CopyResource(&destination,&source);Submit(states);
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
    // 创建并上传 2D 纹理；format 的占用字节由调用方给出，数据按紧密行写入。
    ComPtr<ID3D12Resource> Texture(UINT width,UINT height,DXGI_FORMAT format,UINT bytesPerPixel,
        D3D12_RESOURCE_FLAGS flags,const std::vector<std::uint8_t>& data,const char* name)
    {
        Check(data.size()==std::size_t{width}*height*bytesPerPixel,"texture data size");
        D3D12_RESOURCE_DESC description{};
        description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;description.Width=width;description.Height=height;
        description.DepthOrArraySize=description.MipLevels=1;description.Format=format;description.SampleDesc={1,0};
        description.Flags=flags;
        ComPtr<ID3D12Resource> texture;
        Hr(context.memory->Create(description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,texture,name),name);
        const UINT pitch=RowPitch(width,bytesPerPixel);
        D3D12UploadSlice slice;Hr(uploads.Allocate(UINT64{pitch}*height,512,slice),"TAA upload");
        for(UINT y=0;y<height;++y)
            std::memcpy(slice.cpu+UINT64{y}*pitch,data.data()+std::size_t{y}*width*bytesPerPixel,std::size_t{width}*bytesPerPixel);
        Begin();D3D12StateRecording states(*context.states);
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=texture.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=slice.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset=slice.offset;
        source.PlacedFootprint.Footprint={format,width,height,1,pitch};
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);Submit(states);
        return texture;
    }
    std::vector<float> ReadTexture(ID3D12Resource& texture,const char* name)
    {
        const auto description=texture.GetDesc();
        Check(description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&description.DepthOrArraySize==1,"readback 2D");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 total=0;
        context.device->GetCopyableFootprints(&description,0,1,0,&layout,nullptr,nullptr,&total);
        auto readback=Readback(total,name);
        Begin();D3D12StateRecording states(*context.states);
        Hr(states.Use(*commands.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,"TaaTest.Read"),"TAA read state");
        Hr(states.Use(*commands.Get(),*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"TaaTest.Read"),"TAA read destination");
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=readback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=layout;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=&texture;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);Submit(states);
        void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(total)},none{0,0};
        Hr(readback->Map(0,&range,&mapped),name);
        const UINT width=static_cast<UINT>(description.Width),height=static_cast<UINT>(description.Height);
        const UINT channels=description.Format==DXGI_FORMAT_R16G16_FLOAT?2u:description.Format==DXGI_FORMAT_R32_FLOAT?1u:4u;
        std::vector<float> result(std::size_t{width}*height*channels);
        for(UINT y=0;y<height;++y)
        {
            const auto* row=static_cast<const std::byte*>(mapped)+layout.Offset+UINT64{y}*layout.Footprint.RowPitch;
            for(UINT x=0;x<width;++x)
            {
                float values[4]{0,0,0,1};
                for(UINT channel=0;channel<channels;++channel)
                {
                    if(description.Format==DXGI_FORMAT_R32_FLOAT)
                    {
                        std::memcpy(&values[channel],row+UINT64{x}*4,4);
                    }
                    else
                    {
                        std::uint16_t bits=0;std::memcpy(&bits,row+UINT64{x}*UINT64{channels}*2+UINT64{channel}*2,2);
                        values[channel]=HalfToFloat(bits);
                    }
                }
                for(UINT channel=0;channel<channels;++channel)
                    result[(std::size_t{y}*width+x)*channels+channel]=values[channel];
            }
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
struct Scene
{
    UINT width=0,height=0;
    std::vector<Vec3> current;
    std::vector<float> depth;
    std::vector<Vec2> velocity;
};
std::vector<std::uint8_t> ColorBytes(const std::vector<Vec3>& pixels)
{
    std::vector<std::uint8_t> bytes(pixels.size()*8);
    for(std::size_t index=0;index<pixels.size();++index)
    {
        const std::uint16_t values[4]{FloatToHalf(pixels[index].x),FloatToHalf(pixels[index].y),FloatToHalf(pixels[index].z),FloatToHalf(1.0f)};
        std::memcpy(bytes.data()+index*8,values,8);
    }
    return bytes;
}
std::vector<std::uint8_t> DepthBytes(const std::vector<float>& depth)
{
    std::vector<std::uint8_t> bytes(depth.size()*4);
    std::memcpy(bytes.data(),depth.data(),bytes.size());return bytes;
}
std::vector<std::uint8_t> VelocityBytes(const std::vector<Vec2>& velocity)
{
    std::vector<std::uint8_t> bytes(velocity.size()*4);
    for(std::size_t index=0;index<velocity.size();++index)
    {
        const std::uint16_t values[2]{FloatToHalf(velocity[index].x),FloatToHalf(velocity[index].y)};
        std::memcpy(bytes.data()+index*4,values,4);
    }
    return bytes;
}
TaaResolveInput CpuInput(const Scene& scene,UINT x,UINT y,Vec3 history,float historyDepth,bool historyValid)
{
    TaaResolveInput input;
    const auto clampPixel=[&](int px,int py)->std::pair<UINT,UINT>{
        return {std::min<UINT>(std::max(px,0),scene.width-1),std::min<UINT>(std::max(py,0),scene.height-1)};
    };
    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
    {
        const auto [px,py]=clampPixel(int(x)+dx,int(y)+dy);
        input.neighborhood[std::size_t(dy+1)*3+std::size_t(dx+1)]=scene.current[std::size_t{py}*scene.width+px];
    }
    input.history=history;input.currentDepth=scene.depth[std::size_t{y}*scene.width+x];input.historyDepth=historyDepth;
    input.historyValid=historyValid;
    input.velocity.valid=true;input.velocity.du=scene.velocity[std::size_t{y}*scene.width+x].x;
    input.velocity.dv=scene.velocity[std::size_t{y}*scene.width+x].y;
    return input;
}
void StaticAndDisocclusion(Fixture& fixture,nlohmann::json& evidence)
{
    const UINT width=8,height=8;
    Scene scene;scene.width=width;scene.height=height;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
    {
        const float value=HalfValue(0.3f+0.2f*float(x)/float(width-1));
        scene.current.push_back({value,value,value});
        scene.depth.push_back(0.5f);
        scene.velocity.push_back({0,0});
    }
    auto hdr=fixture.Texture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,8,{},ColorBytes(scene.current),"TaaTest.Color");
    auto depth=fixture.Texture(width,height,DXGI_FORMAT_D32_FLOAT,4,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        DepthBytes(scene.depth),"TaaTest.Depth");
    auto velocity=fixture.Texture(width,height,DXGI_FORMAT_R16G16_FLOAT,4,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        VelocityBytes(scene.velocity),"TaaTest.Velocity");
    D3D12TaaOutput output;Hr(fixture.kernel.CreateOutput(*hdr.Get(),*depth.Get(),width,height,output),"TAA output");
    fixture.CopyTexture(*output.velocity.Get(),*velocity.Get());
    const auto fillHistory=[&](D3D12TaaOutput& target,UINT index,Vec3 color,float value)
    {
        auto colorData=fixture.Texture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,8,{},
            ColorBytes(std::vector<Vec3>(std::size_t{width}*height,color)),"TaaTest.HistoryColor");
        auto depthData=fixture.Texture(width,height,DXGI_FORMAT_R32_FLOAT,4,{},
            DepthBytes(std::vector<float>(std::size_t{width}*height,value)),"TaaTest.HistoryDepth");
        fixture.CopyTexture(*target.historyColor[index].Get(),*colorData.Get());
        fixture.CopyTexture(*target.historyDepth[index].Get(),*depthData.Get());
    };
    const auto component=[](Vec3 value,int channel){return channel==0?value.x:channel==1?value.y:value.z;};
    D3D12TaaParameters parameters;parameters.width=float(width);parameters.height=float(height);parameters.historyValid=1.0f;
    fillHistory(output,output.historyIndex,Vec3{0.45f,0.45f,0.45f},0.5f);
    fixture.Begin();D3D12StateRecording states(*fixture.context.states);
    Hr(fixture.kernel.Resolve(*fixture.commands.Get(),states,output,*hdr.Get(),*depth.Get(),parameters),"TAA static resolve");
    fixture.Submit(states);
    const auto staticColor=fixture.ReadTexture(*output.historyColor[output.historyIndex].Get(),"TAA static color");
    const auto staticDepth=fixture.ReadTexture(*output.historyDepth[output.historyIndex].Get(),"TAA static depth");
    // 与 CPU ResolveTaa 逐像素对照：当前帧为 0.3..0.5 梯度，历史 0.45 可落入邻域。
    TaaSettings cpuSettings;float staticError=0;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
    {
        const auto input=CpuInput(scene,x,y,Vec3{HalfValue(0.45f),HalfValue(0.45f),HalfValue(0.45f)},0.5f,true);
        const auto expected=ResolveTaa(input,cpuSettings);
        for(int channel=0;channel<3;++channel)
        {
            const auto reference=HalfValue(channel==0?expected.color.x:channel==1?expected.color.y:expected.color.z);
            staticError=std::max(staticError,std::abs(staticColor[(std::size_t{y}*width+x)*4+channel]-reference));
        }
        Check(std::abs(staticDepth[std::size_t{y}*width+x]-HalfValue(0.5f))<1e-6f,"resolve writes current depth into history");
    }
    Check(staticError<2e-3f,"static history blend matches CPU ResolveTaa");
    // 深度 disocclusion：历史更远，直接接受当前帧。
    fillHistory(output,output.historyIndex,Vec3{0.8f,0.8f,0.8f},0.7f);
    fixture.Begin();D3D12StateRecording disocclusionStates(*fixture.context.states);
    Hr(fixture.kernel.Resolve(*fixture.commands.Get(),disocclusionStates,output,*hdr.Get(),*depth.Get(),parameters),"TAA depth disocclusion");
    fixture.Submit(disocclusionStates);
    const auto depthResult=fixture.ReadTexture(*output.historyColor[output.historyIndex].Get(),"TAA depth disocclusion");
    Check(std::abs(depthResult[0]-scene.current[0].x)<1e-3f,"depth discontinuity rejects history");
    // 速度 disocclusion：使用独立输出与 0.02 UV 速度。
    D3D12TaaOutput movingOutput;Hr(fixture.kernel.CreateOutput(*hdr.Get(),*depth.Get(),width,height,movingOutput),"TAA moving output");
    const std::vector<Vec2> movingVelocity(std::size_t{width}*height,Vec2{0.02f,0.0f});
    auto movingTexture=fixture.Texture(width,height,DXGI_FORMAT_R16G16_FLOAT,4,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        VelocityBytes(movingVelocity),"TaaTest.MovingVelocity");
    fixture.CopyTexture(*movingOutput.velocity.Get(),*movingTexture.Get());
    fillHistory(movingOutput,0,Vec3{0.45f,0.45f,0.45f},0.5f);
    fixture.Begin();D3D12StateRecording movingResolve(*fixture.context.states);
    Hr(fixture.kernel.Resolve(*fixture.commands.Get(),movingResolve,movingOutput,*hdr.Get(),*depth.Get(),parameters),"TAA velocity disocclusion");
    fixture.Submit(movingResolve);
    const auto velocityResult=fixture.ReadTexture(*movingOutput.historyColor[movingOutput.historyIndex].Get(),"TAA velocity disocclusion");
    Check(std::abs(velocityResult[0]-scene.current[0].x)<1e-3f,"velocity above threshold rejects history");
    // 历史失效。
    parameters.historyValid=0;
    fixture.Begin();D3D12StateRecording invalidStates(*fixture.context.states);
    Hr(fixture.kernel.Resolve(*fixture.commands.Get(),invalidStates,output,*hdr.Get(),*depth.Get(),parameters),"TAA invalid history");
    fixture.Submit(invalidStates);
    const auto invalidResult=fixture.ReadTexture(*output.historyColor[output.historyIndex].Get(),"TAA invalid history");
    Check(std::abs(invalidResult[0]-scene.current[0].x)<1e-3f,"invalid history accepts the current frame");
    evidence["static"]={{"error",staticError},{"actual",staticColor[0]},{"history_depth",staticDepth[0]}};
    evidence["disocclusion"]={{"depth",depthResult[0]},{"velocity",velocityResult[0]},{"invalid",invalidResult[0]}};
    std::cout<<"TAA static error="<<staticError<<" actual="<<staticColor[0]
        <<", depth/velocity disocclusion="<<depthResult[0]<<"/"<<velocityResult[0]<<'\n';
}
void ClampAgainstCpu(Fixture& fixture,nlohmann::json& evidence)
{
    const UINT width=12,height=10;
    Scene scene;scene.width=width;scene.height=height;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
    {
        const float value=0.2f+0.5f*float(x)/float(width-1);
        scene.current.push_back({value,value*0.5f,1.0f-value});
        scene.depth.push_back(0.4f+0.01f*float(y));
        scene.velocity.push_back({0,0});
    }
    for(auto& color:scene.current)color=HalfColor(color);
    auto hdr=fixture.Texture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,8,{},ColorBytes(scene.current),"TaaClamp.Color");
    auto depth=fixture.Texture(width,height,DXGI_FORMAT_D32_FLOAT,4,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        DepthBytes(scene.depth),"TaaClamp.Depth");
    auto velocity=fixture.Texture(width,height,DXGI_FORMAT_R16G16_FLOAT,4,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        VelocityBytes(scene.velocity),"TaaClamp.Velocity");
    D3D12TaaOutput output;Hr(fixture.kernel.CreateOutput(*hdr.Get(),*depth.Get(),width,height,output),"TAA clamp output");
    fixture.CopyTexture(*output.velocity.Get(),*velocity.Get());
    const std::vector<Vec3> historyColors(std::size_t{width}*height,Vec3{2.0f,2.0f,2.0f});
    auto historyColor=fixture.Texture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,8,{},ColorBytes(historyColors),"TaaClamp.History");
    auto historyDepth=fixture.Texture(width,height,DXGI_FORMAT_R32_FLOAT,4,{},DepthBytes(scene.depth),"TaaClamp.HistoryDepth");
    fixture.CopyTexture(*output.historyColor[output.historyIndex].Get(),*historyColor.Get());
    fixture.CopyTexture(*output.historyDepth[output.historyIndex].Get(),*historyDepth.Get());
    D3D12TaaParameters parameters;parameters.width=float(width);parameters.height=float(height);parameters.historyValid=1.0f;
    fixture.Begin();D3D12StateRecording resolveState(*fixture.context.states);
    Hr(fixture.kernel.Resolve(*fixture.commands.Get(),resolveState,output,*hdr.Get(),*depth.Get(),parameters),"TAA clamp resolve");
    fixture.Submit(resolveState);
    const auto actual=fixture.ReadTexture(*output.historyColor[output.historyIndex].Get(),"TaaClamp.Result");
    TaaSettings cpuSettings;
    float maximumError=0;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
    {
        const auto input=CpuInput(scene,x,y,Vec3{2.0f,2.0f,2.0f},scene.depth[std::size_t{y}*width+x],true);
        const auto expected=ResolveTaa(input,cpuSettings);
        for(int channel=0;channel<3;++channel)
        {
            const auto reference=HalfValue(channel==0?expected.color.x:channel==1?expected.color.y:expected.color.z);
            maximumError=std::max(maximumError,std::abs(actual[(std::size_t{y}*width+x)*4+channel]-reference));
        }
    }
    Check(maximumError<2e-3f,"GPU clamp/resolve matches CPU ResolveTaa on a gradient");
    evidence["clamp"]={{"max_error",maximumError}};
    std::cout<<"TAA clamp max error="<<maximumError<<'\n';
}
void Contracts(Fixture& fixture)
{
    const UINT width=8,height=8;
    Scene scene;scene.width=width;scene.height=height;
    scene.current.assign(std::size_t{width}*height,Vec3{0.5f,0.5f,0.5f});
    scene.depth.assign(std::size_t{width}*height,0.5f);
    scene.velocity.assign(std::size_t{width}*height,Vec2{0,0});
    auto hdr=fixture.Texture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,8,{},ColorBytes(scene.current),"TaaContract.Color");
    auto depth=fixture.Texture(width,height,DXGI_FORMAT_D32_FLOAT,4,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        DepthBytes(scene.depth),"TaaContract.Depth");
    D3D12TaaOutput output;
    D3D12Taa uninitialized;
    Check(uninitialized.CreateOutput(*hdr.Get(),*depth.Get(),width,height,output)==E_UNEXPECTED,"uninitialized TAA kernel rejected");
    Hr(fixture.kernel.CreateOutput(*hdr.Get(),*depth.Get(),width,height,output),"TAA contract output");
    D3D12TaaOutput failed;
    const auto* old=output.velocity.Get();
    Check(fixture.kernel.CreateOutput(*hdr.Get(),*depth.Get(),4,4,failed)==E_INVALIDARG&&!failed.velocity,"wrong TAA size rejected");
    fixture.Begin();
    D3D12StateRecording states(*fixture.context.states);
    D3D12TaaParameters parameters;parameters.historyValid=1.0f;
    D3D12TaaOutput empty;
    Check(fixture.kernel.Resolve(*fixture.commands.Get(),states,empty,*hdr.Get(),*depth.Get(),parameters)==E_INVALIDARG
        &&states.PendingTransitions()==0,"empty TAA output rejected before transitions");
    fixture.Submit(states);
    Check(output.historyIndex==0,"TAA initial history index");
    D3D12TaaOutput second;Hr(fixture.kernel.CreateOutput(*hdr.Get(),*depth.Get(),width,height,second),"TAA second output");
    fixture.Begin();D3D12StateRecording firstState(*fixture.context.states);
    Hr(fixture.kernel.Resolve(*fixture.commands.Get(),firstState,second,*hdr.Get(),*depth.Get(),parameters),"TAA first resolve");
    Check(second.historyIndex==1&&second.frames==1,"TAA first resolve flips history index");
    fixture.Submit(firstState);
    Check(output.velocity.Get()==old,"successful output preserved");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply TAA output directory");
        const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"TAA localization");
        Fixture fixture(text);
        nlohmann::json evidence;
        StaticAndDisocclusion(fixture,evidence);
        ClampAgainstCpu(fixture,evidence);
        Contracts(fixture);
        fixture.Shutdown();
        Check(fixture.context.states->Stats().resources==0&&fixture.context.memory->Stats().used==0,"all TAA allocations released");
        std::ofstream report(output/"validation.json");report<<evidence.dump(2)<<'\n';Check(bool(report),"TAA evidence output");
        std::cout<<"GpuTaaTests passed (static blend, depth/velocity disocclusion, invalid history, CPU clamp oracle, contracts).\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<"GpuTaaTests: "<<error.what()<<'\n';return 1;}
}
