#include "D3D12Visibility.h"
#include "D3D12Context.h"
#include "D3D12Memory.h"
#include "D3D12Shader.h"
#include "D3D12StateTracker.h"
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace
{
struct alignas(256) Parameters
{
    se::VisibilityFrustum frustum;
    UINT count,capacity,reserved[2]{};
    std::byte padding[144]{};
};
static_assert(sizeof(Parameters)==256&&offsetof(Parameters,count)==96);
D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
    d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc={1,0};d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;return d;
}
}
HRESULT D3D12Visibility::Initialize(D3D12Context& context,const std::filesystem::path& shader,const Localization& text)
{
    if(device_)return E_UNEXPECTED;
    if(!context.device||!context.states||!context.memory)return E_INVALIDARG;
    ComPtr<ID3DBlob> resetCode,cullCode;
    if(!CompileD3D12Shader(shader,"Reset","cs_5_1",resetCode,text,D3DCOMPILE_IEEE_STRICTNESS)
        ||!CompileD3D12Shader(shader,"Cull","cs_5_1",cullCode,text,D3DCOMPILE_IEEE_STRICTNESS))return E_FAIL;
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[0].Descriptor.ShaderRegister=0;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor.ShaderRegister=0;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor.ShaderRegister=0;
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor.ShaderRegister=1;
    D3D12_ROOT_SIGNATURE_DESC description{};description.NumParameters=4;description.pParameters=parameters;
    ComPtr<ID3DBlob> signature,error;
    auto result=D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,signature.GetAddressOf(),error.GetAddressOf());
    if(FAILED(result))return result;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> reset,cull;
    result=context.device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(root.GetAddressOf()));
    if(FAILED(result))return result;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();pipeline.CS={resetCode->GetBufferPointer(),resetCode->GetBufferSize()};
    result=context.device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(reset.GetAddressOf()));if(FAILED(result))return result;
    pipeline.CS={cullCode->GetBufferPointer(),cullCode->GetBufferSize()};
    result=context.device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(cull.GetAddressOf()));if(FAILED(result))return result;
    device_=context.device;memory_=context.memory;root_=std::move(root);reset_=std::move(reset);cull_=std::move(cull);return S_OK;
}
HRESULT D3D12Visibility::CreateOutput(UINT capacity,D3D12VisibilityOutput& output) const
{
    if(!device_)return E_UNEXPECTED;if(capacity>se::MaximumVisibilityItems)return E_INVALIDARG;
    D3D12VisibilityOutput candidate;candidate.capacity=capacity;
    auto result=memory_->Create(Buffer((UINT64{capacity}+2)*sizeof(UINT)),D3D12_RESOURCE_STATE_COMMON,nullptr,candidate.ids,"Visibility.Ids");
    if(FAILED(result))return result;
    result=memory_->Create(Buffer(16),D3D12_RESOURCE_STATE_COMMON,nullptr,candidate.counters,"Visibility.Counters");
    if(FAILED(result))return result;output=std::move(candidate);return S_OK;
}
HRESULT D3D12Visibility::Prepare(D3D12UploadArena& arena,std::span<const se::VisibilityItem> items,const se::VisibilityFrustum& frustum,
    UINT capacity,D3D12VisibilityPrepared& output) const
{
    if(!device_)return E_UNEXPECTED;
    if(items.size()>se::MaximumVisibilityItems||capacity>se::MaximumVisibilityItems||!se::ValidVisibilityFrustum(frustum))return E_INVALIDARG;
    D3D12VisibilityPrepared next;next.count=static_cast<UINT>(items.size());next.capacity=capacity;
    auto result=arena.Allocate(std::max<std::size_t>(sizeof(se::VisibilityItem),items.size_bytes()),16,next.items);if(FAILED(result))return result;
    result=arena.Allocate(sizeof(Parameters),256,next.constants);if(FAILED(result))return result;
    if(!items.empty())std::memcpy(next.items.cpu,items.data(),items.size_bytes());else std::memset(next.items.cpu,0,sizeof(se::VisibilityItem));
    Parameters parameters{};parameters.frustum=frustum;parameters.count=next.count;parameters.capacity=capacity;
    std::memcpy(next.constants.cpu,&parameters,sizeof(parameters));output=next;return S_OK;
}
HRESULT D3D12Visibility::Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,const D3D12VisibilityPrepared& p,
    const D3D12VisibilityOutput& output,D3D12VisibilityTimestamps timestamps,UINT firstQuery) const
{
    if(!device_)return E_UNEXPECTED;
    if(!p.items.resource||!p.constants.resource||!output.ids||!output.counters||p.capacity>output.capacity
        ||output.capacity>se::MaximumVisibilityItems||output.ids->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER
        ||output.counters->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER
        ||output.ids->GetDesc().Width<(UINT64{output.capacity}+2)*4||output.counters->GetDesc().Width<16)return E_INVALIDARG;
    if(timestamps.heap)
    {
        ComPtr<ID3D12Device> owner;
        if(firstQuery>timestamps.capacity||timestamps.capacity-firstQuery<3
            ||FAILED(timestamps.heap->GetDevice(IID_PPV_ARGS(owner.GetAddressOf())))||owner.Get()!=device_.Get())return E_INVALIDARG;
    }
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery);
    auto result=states.Use(commands,*p.items.resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Visibility.Input");if(FAILED(result))return result;
    result=states.Use(commands,*p.constants.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Visibility.Parameters");if(FAILED(result))return result;
    result=states.Use(commands,*output.ids.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Visibility.Reset");if(FAILED(result))return result;
    result=states.Use(commands,*output.counters.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Visibility.Reset");if(FAILED(result))return result;
    commands.SetComputeRootSignature(root_.Get());commands.SetComputeRootConstantBufferView(0,p.constants.Gpu());commands.SetComputeRootShaderResourceView(1,p.items.Gpu());
    commands.SetComputeRootUnorderedAccessView(2,output.ids->GetGPUVirtualAddress());commands.SetComputeRootUnorderedAccessView(3,output.counters->GetGPUVirtualAddress());
    commands.SetPipelineState(reset_.Get());commands.Dispatch(1,1,1);
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery+1);
    result=states.UavBarrier(commands,*output.ids.Get(),"Visibility.ResetToCull");if(FAILED(result))return result;
    result=states.UavBarrier(commands,*output.counters.Get(),"Visibility.ResetToCull");if(FAILED(result))return result;
    commands.SetPipelineState(cull_.Get());commands.Dispatch(std::max(1u,(p.count+63)/64),1,1);
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery+2);return S_OK;
}
