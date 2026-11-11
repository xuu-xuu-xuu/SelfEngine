#include "D3D12Indirect.h"
#include "D3D12Context.h"
#include "D3D12Memory.h"
#include "D3D12Shader.h"
#include "D3D12StateTracker.h"
#include "CoreLayer/Math/Projection.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace
{
struct alignas(256) Parameters
{
    se::VisibilityFrustum frustum;                  // 0..95
    UINT count,capacity,indexCount,reserved=0;      // 96..111
    se::Mat4 clipFromWorld;                         // 112..175
    float screenWidth,screenHeight,depthBias,mipCount; // 176..191（mipCount 也以 float 写入 cbuffer）
    UINT occlusionEnabled;UINT occlusionPadding[15]{};      // 192..255
};
static_assert(sizeof(Parameters)==256&&offsetof(Parameters,count)==96);
static_assert(offsetof(Parameters,clipFromWorld)==112&&offsetof(Parameters,screenWidth)==176
    &&offsetof(Parameters,occlusionEnabled)==192);
static_assert(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)==20&&se::rhi::InstanceGpuStride==240);
UINT Blocks(UINT count){return std::max(1u,(count+63)/64);}
D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
    d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc={1,0};d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;return d;
}
bool HasBytes(ID3D12Resource* resource,UINT64 bytes)
{return resource&&resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&resource->GetDesc().Width>=bytes;}
bool ValidOcclusion(const D3D12IndirectOcclusion& occlusion)
{
    return occlusion.heap&&occlusion.srv.ptr&&occlusion.width&&occlusion.height
        &&occlusion.width<=8192&&occlusion.height<=8192&&occlusion.mipCount>=1&&occlusion.mipCount<=16
        &&se::IsFinite(occlusion.clipFromWorld)&&std::isfinite(occlusion.depthBias)
        &&occlusion.depthBias>=0&&occlusion.depthBias<=0.1f;
}
}
HRESULT D3D12Indirect::Initialize(D3D12Context& context,const std::filesystem::path& shader,const Localization& text)
{
    if(device_)return E_UNEXPECTED;if(!context.device||!context.states||!context.memory)return E_INVALIDARG;
    std::array<ComPtr<ID3DBlob>,3> code;
    constexpr std::array entries{"LocalScan","GroupScan","Gather"};
    for(size_t i=0;i<code.size();++i)if(!CompileD3D12Shader(shader,entries[i],"cs_5_1",code[i],text,D3DCOMPILE_IEEE_STRICTNESS))return E_FAIL;
    D3D12_DESCRIPTOR_RANGE hzbRange{};
    hzbRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;hzbRange.NumDescriptors=1;hzbRange.BaseShaderRegister=3;
    hzbRange.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[8]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    for(UINT i=1;i<3;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[i].Descriptor.ShaderRegister=i-1;}
    for(UINT i=3;i<7;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[i].Descriptor.ShaderRegister=i-3;}
    parameters[7].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[7].DescriptorTable={1,&hzbRange};parameters[7].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC description{};description.NumParameters=8;description.pParameters=parameters;
    ComPtr<ID3DBlob> serialized,error;
    auto result=D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,serialized.GetAddressOf(),error.GetAddressOf());if(FAILED(result))return result;
    ComPtr<ID3D12RootSignature> root;
    result=context.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(root.GetAddressOf()));if(FAILED(result))return result;
    std::array<ComPtr<ID3D12PipelineState>,3> pipelines;
    for(size_t i=0;i<pipelines.size();++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={code[i]->GetBufferPointer(),code[i]->GetBufferSize()};
        result=context.device->CreateComputePipelineState(&p,IID_PPV_ARGS(pipelines[i].GetAddressOf()));if(FAILED(result))return result;
    }
    D3D12_INDIRECT_ARGUMENT_DESC argument{};argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC command{};command.ByteStride=20;command.NumArgumentDescs=1;command.pArgumentDescs=&argument;
    ComPtr<ID3D12CommandSignature> signature;
    result=context.device->CreateCommandSignature(&command,nullptr,IID_PPV_ARGS(signature.GetAddressOf()));if(FAILED(result))return result;
    device_=context.device;memory_=context.memory;root_=std::move(root);signature_=std::move(signature);
    local_=std::move(pipelines[0]);groups_=std::move(pipelines[1]);gather_=std::move(pipelines[2]);return S_OK;
}
HRESULT D3D12Indirect::CreateOutput(UINT inputCapacity,UINT outputCapacity,D3D12IndirectOutput& output) const
{
    if(!device_)return E_UNEXPECTED;
    if(inputCapacity>se::MaximumVisibilityItems||outputCapacity>se::MaximumVisibilityItems)return E_INVALIDARG;
    D3D12IndirectOutput next;next.inputCapacity=inputCapacity;next.outputCapacity=outputCapacity;
    auto result=memory_->Create(Buffer(UINT64{std::max(1u,inputCapacity)}*4),D3D12_RESOURCE_STATE_COMMON,nullptr,next.localOffsets,"Indirect.LocalOffsets");if(FAILED(result))return result;
    result=memory_->Create(Buffer(UINT64{Blocks(inputCapacity)}*4),D3D12_RESOURCE_STATE_COMMON,nullptr,next.blockOffsets,"Indirect.BlockOffsets");if(FAILED(result))return result;
    result=memory_->Create(Buffer(UINT64{outputCapacity}*se::rhi::InstanceGpuStride+8),D3D12_RESOURCE_STATE_COMMON,nullptr,next.instances,"Indirect.Instances");if(FAILED(result))return result;
    result=memory_->Create(Buffer(argumentBytes),D3D12_RESOURCE_STATE_COMMON,nullptr,next.arguments,"Indirect.Arguments");if(FAILED(result))return result;
    output=std::move(next);return S_OK;
}
HRESULT D3D12Indirect::Prepare(D3D12UploadArena& arena,std::span<const se::VisibilityItem> items,const se::VisibilityFrustum& frustum,
    D3D12UploadSlice instances,UINT indexCount,UINT capacity,D3D12IndirectPrepared& output,
    const D3D12IndirectOcclusion* occlusion) const
{
    if(!device_)return E_UNEXPECTED;
    if(items.size()>se::MaximumVisibilityItems||capacity>se::MaximumVisibilityItems||!indexCount||!se::ValidVisibilityFrustum(frustum)
        ||(occlusion&&!ValidOcclusion(*occlusion))||(occlusion&&capacity<items.size()))return E_INVALIDARG;
    const auto bytes=UINT64{items.size()}*se::rhi::InstanceGpuStride;
    if(bytes&&(!instances.resource||instances.size<bytes||instances.offset%4||!HasBytes(instances.resource,instances.offset)
        ||instances.resource->GetDesc().Width-instances.offset<bytes))return E_INVALIDARG;
    D3D12IndirectPrepared next;next.count=static_cast<UINT>(items.size());next.capacity=capacity;next.instances=instances;
    next.occlusion=occlusion!=nullptr;
    if(!bytes)
    {
        const auto result=arena.Allocate(se::rhi::InstanceGpuStride,16,next.instances);if(FAILED(result))return result;
        std::memset(next.instances.cpu,0,se::rhi::InstanceGpuStride);
    }
    auto result=arena.Allocate(std::max<std::size_t>(sizeof(se::VisibilityItem),items.size_bytes()),16,next.bounds);if(FAILED(result))return result;
    result=arena.Allocate(sizeof(Parameters),256,next.constants);if(FAILED(result))return result;
    if(!items.empty())std::memcpy(next.bounds.cpu,items.data(),items.size_bytes());else std::memset(next.bounds.cpu,0,sizeof(se::VisibilityItem));
    Parameters parameters{};parameters.frustum=frustum;parameters.count=next.count;parameters.capacity=capacity;parameters.indexCount=indexCount;
    if(occlusion)
    {
        parameters.clipFromWorld=occlusion->clipFromWorld;
        parameters.screenWidth=float(occlusion->width);parameters.screenHeight=float(occlusion->height);
        parameters.depthBias=occlusion->depthBias;parameters.mipCount=float(occlusion->mipCount);
        parameters.occlusionEnabled=1;
    }
    std::memcpy(next.constants.cpu,&parameters,sizeof(parameters));output=next;return S_OK;
}
HRESULT D3D12Indirect::Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,const D3D12IndirectPrepared& p,
    const D3D12IndirectOutput& o,D3D12VisibilityTimestamps timestamps,UINT firstQuery,
    const D3D12IndirectOcclusion* occlusion) const
{
    if(!device_)return E_UNEXPECTED;
    if(!p.bounds.resource||!p.instances.resource||!p.constants.resource||o.inputCapacity>se::MaximumVisibilityItems||o.outputCapacity>se::MaximumVisibilityItems
        ||p.count>o.inputCapacity||p.capacity>o.outputCapacity||!HasBytes(o.localOffsets.Get(),UINT64{std::max(1u,o.inputCapacity)}*4)
        ||!HasBytes(o.blockOffsets.Get(),UINT64{Blocks(o.inputCapacity)}*4)||!HasBytes(o.instances.Get(),UINT64{o.outputCapacity}*se::rhi::InstanceGpuStride+8)
        ||!HasBytes(o.arguments.Get(),argumentBytes)||p.occlusion!=(occlusion!=nullptr)||(occlusion&&!ValidOcclusion(*occlusion)))return E_INVALIDARG;
    if(timestamps.heap)
    {
        ComPtr<ID3D12Device> owner;
        if(firstQuery>timestamps.capacity||timestamps.capacity-firstQuery<4||FAILED(timestamps.heap->GetDevice(IID_PPV_ARGS(owner.GetAddressOf())))||owner.Get()!=device_.Get())return E_INVALIDARG;
        commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery);
    }
    auto result=states.Use(commands,*p.bounds.resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indirect.Bounds");if(FAILED(result))return result;
    result=states.Use(commands,*p.instances.resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indirect.SourceInstances");if(FAILED(result))return result;
    result=states.Use(commands,*p.constants.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Indirect.Constants");if(FAILED(result))return result;
    for(auto* resource:{o.localOffsets.Get(),o.blockOffsets.Get(),o.instances.Get(),o.arguments.Get()})
    {result=states.Use(commands,*resource,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Indirect.ScanGather");if(FAILED(result))return result;}
    commands.SetComputeRootSignature(root_.Get());commands.SetComputeRootConstantBufferView(0,p.constants.Gpu());
    commands.SetComputeRootShaderResourceView(1,p.bounds.Gpu());commands.SetComputeRootShaderResourceView(2,p.instances.Gpu());
    commands.SetComputeRootUnorderedAccessView(3,o.localOffsets->GetGPUVirtualAddress());commands.SetComputeRootUnorderedAccessView(4,o.blockOffsets->GetGPUVirtualAddress());
    commands.SetComputeRootUnorderedAccessView(5,o.instances->GetGPUVirtualAddress());commands.SetComputeRootUnorderedAccessView(6,o.arguments->GetGPUVirtualAddress());
    if(occlusion)
    {
        ID3D12DescriptorHeap* heaps[]{occlusion->heap};
        commands.SetDescriptorHeaps(1,heaps);commands.SetComputeRootDescriptorTable(7,occlusion->srv);
    }
    commands.SetPipelineState(local_.Get());commands.Dispatch(Blocks(p.count),1,1);
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery+1);
    result=states.UavBarrier(commands,*o.localOffsets.Get(),"Indirect.LocalToGather");if(FAILED(result))return result;
    result=states.UavBarrier(commands,*o.blockOffsets.Get(),"Indirect.LocalToGroups");if(FAILED(result))return result;
    commands.SetPipelineState(groups_.Get());commands.Dispatch(1,1,1);
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery+2);
    result=states.UavBarrier(commands,*o.blockOffsets.Get(),"Indirect.GroupsToGather");if(FAILED(result))return result;
    result=states.UavBarrier(commands,*o.instances.Get(),"Indirect.GuardToGather");if(FAILED(result))return result;
    commands.SetPipelineState(gather_.Get());commands.Dispatch(Blocks(p.count),1,1);
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery+3);return S_OK;
}
