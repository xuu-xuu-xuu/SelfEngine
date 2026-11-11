#include "D3D12HiZ.h"
#include "D3D12Context.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include "D3D12Shader.h"
#include "D3D12StateTracker.h"
#include "CoreLayer/Localization.h"
#include <algorithm>
#include <array>

using Microsoft::WRL::ComPtr;
namespace
{
// 根常量与 HLSL cbuffer 的前四个 uint 对应，避免为每级复制常量而借用上传页。
struct alignas(16) LevelParameters {UINT sourceWidth,sourceHeight,destinationWidth,destinationHeight;};
static_assert(sizeof(LevelParameters)==16);
bool ValidDepth(const D3D12_RESOURCE_DESC& description,UINT width,UINT height)
{
    return description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&description.Width==width
        &&description.Height==height&&description.DepthOrArraySize==1&&description.MipLevels==1
        &&description.SampleDesc.Count==1&&(description.Format==DXGI_FORMAT_R32_FLOAT||description.Format==DXGI_FORMAT_D32_FLOAT);
}
D3D12_CPU_DESCRIPTOR_HANDLE Offset(D3D12_CPU_DESCRIPTOR_HANDLE handle,UINT increment,UINT index)
{
    handle.ptr+=static_cast<SIZE_T>(increment)*index;return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE Offset(D3D12_GPU_DESCRIPTOR_HANDLE handle,UINT increment,UINT index)
{
    handle.ptr+=static_cast<UINT64>(increment)*index;return handle;
}
}
UINT D3D12HiZ::Levels(UINT width,UINT height)
{
    if(!width||!height||width>8192||height>8192)return 0;
    UINT levels=1;
    while(width>1||height>1)
    {
        width=(std::max)(1u,width/2);height=(std::max)(1u,height/2);
        if(++levels>maximumLevels)return 0;
    }
    return levels;
}
HRESULT D3D12HiZ::Initialize(D3D12Context& context,const std::filesystem::path& shader,const Localization& text)
{
    if(device_)return E_UNEXPECTED;
    if(!context.device||!context.memory)return E_INVALIDARG;
    ComPtr<ID3DBlob> copyCode,reduceCode;
    if(!CompileD3D12Shader(shader,"Copy","cs_5_1",copyCode,text,D3DCOMPILE_IEEE_STRICTNESS)
        ||!CompileD3D12Shader(shader,"Reduce","cs_5_1",reduceCode,text,D3DCOMPILE_IEEE_STRICTNESS))
    {CheckGraphicsResult(E_FAIL,text,"D3D12.HiZShaderFailed");return E_FAIL;}
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=1;
    ranges[0].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=1;
    ranges[1].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[2]=ranges[1];
    ranges[2].BaseShaderRegister=1;
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,4};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={1,&ranges[0]};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable={1,&ranges[1]};parameters[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable={1,&ranges[2]};parameters[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters=4;description.pParameters=parameters;
    description.Flags=D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> signature,error;
    auto result=D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,signature.GetAddressOf(),error.GetAddressOf());
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> copy,reduce;
    if(SUCCEEDED(result))result=context.device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),
        IID_PPV_ARGS(root.GetAddressOf()));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    if(SUCCEEDED(result))
    {
        pipeline.pRootSignature=root.Get();pipeline.CS={copyCode->GetBufferPointer(),copyCode->GetBufferSize()};
        result=context.device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(copy.GetAddressOf()));
    }
    if(SUCCEEDED(result))
    {
        pipeline.CS={reduceCode->GetBufferPointer(),reduceCode->GetBufferSize()};
        result=context.device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(reduce.GetAddressOf()));
    }
    if(FAILED(result)){CheckGraphicsResult(result,text,"D3D12.HiZShaderFailed");return result;}
    device_=context.device;memory_=context.memory;root_=std::move(root);copy_=std::move(copy);reduce_=std::move(reduce);return S_OK;
}
HRESULT D3D12HiZ::CreateOutput(ID3D12Resource& depth,UINT width,UINT height,D3D12HiZOutput& output) const
{
    if(!device_||!memory_)return E_UNEXPECTED;
    const auto levels=Levels(width,height);
    if(!levels||!ValidDepth(depth.GetDesc(),width,height))return E_INVALIDARG;
    D3D12HiZOutput candidate;candidate.width=width;candidate.height=height;candidate.mipCount=levels;candidate.depth=&depth;
    D3D12_RESOURCE_DESC hzb{};
    hzb.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;hzb.Width=width;hzb.Height=height;hzb.DepthOrArraySize=1;
    hzb.MipLevels=static_cast<UINT16>(levels);hzb.Format=DXGI_FORMAT_R32_FLOAT;hzb.SampleDesc={1,0};
    hzb.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;hzb.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto result=memory_->Create(hzb,D3D12_RESOURCE_STATE_COMMON,nullptr,candidate.texture,"HiZ.Texture");
    if(FAILED(result))return result;
    const UINT slots=2+levels;
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.NumDescriptors=slots;heapDescription.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;heapDescription.NodeMask=1;
    result=device_->CreateDescriptorHeap(&heapDescription,IID_PPV_ARGS(candidate.descriptors.GetAddressOf()));
    if(FAILED(result))return result;
    candidate.descriptors->SetName(L"SelfEngine.HiZDescriptors");
    const auto increment=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu=candidate.descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto gpu=candidate.descriptors->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format=DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D={0,1,0,0};
    device_->CreateShaderResourceView(&depth,&srv,Offset(cpu,increment,0));
    srv.Texture2D.MipLevels=levels;
    device_->CreateShaderResourceView(candidate.texture.Get(),&srv,Offset(cpu,increment,1));
    candidate.depthSrv=Offset(gpu,increment,0);candidate.hzbSrv=Offset(gpu,increment,1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    for(UINT level=0;level<levels;++level)
    {
        uav.Texture2D={level,0};
        device_->CreateUnorderedAccessView(candidate.texture.Get(),nullptr,&uav,Offset(cpu,increment,2+level));
        candidate.levels[level]=Offset(gpu,increment,2+level);
    }
    output=std::move(candidate);return S_OK;
}
HRESULT D3D12HiZ::Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
    const D3D12HiZOutput& output,D3D12HiZTimestamps timestamps,UINT firstQuery) const
{
    if(!device_)return E_UNEXPECTED;
    if(!output.texture||!output.depth||!output.descriptors||!output.mipCount||output.mipCount>maximumLevels
        ||!output.width||!output.height||output.width>8192||output.height>8192)return E_INVALIDARG;
    const auto description=output.texture->GetDesc();
    if(description.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||description.Width!=output.width
        ||description.Height!=output.height||description.MipLevels!=output.mipCount||description.Format!=DXGI_FORMAT_R32_FLOAT
        ||Levels(output.width,output.height)!=output.mipCount||!ValidDepth(output.depth->GetDesc(),output.width,output.height))
        return E_INVALIDARG;
    if(timestamps.heap)
    {
        ComPtr<ID3D12Device> owner;
        if(firstQuery>timestamps.capacity||timestamps.capacity-firstQuery<queryCount
            ||FAILED(timestamps.heap->GetDevice(IID_PPV_ARGS(owner.GetAddressOf())))||owner.Get()!=device_.Get())
            return E_INVALIDARG;
    }
    std::array<UINT,maximumLevels> widths{},heights{};
    widths[0]=output.width;heights[0]=output.height;
    UINT count=1;
    while((widths[count-1]>1||heights[count-1]>1)&&count<maximumLevels)
    {
        widths[count]=(std::max)(1u,widths[count-1]/2);
        heights[count]=(std::max)(1u,heights[count-1]/2);
        ++count;
    }
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery);
    auto result=states.Use(commands,*output.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"HiZ.Depth");
    if(FAILED(result))return result;
    result=states.Use(commands,*output.texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"HiZ.Build");
    if(FAILED(result))return result;
    commands.SetComputeRootSignature(root_.Get());
    ID3D12DescriptorHeap* heaps[]{output.descriptors.Get()};
    commands.SetDescriptorHeaps(1,heaps);
    commands.SetComputeRootDescriptorTable(1,output.depthSrv);
    const auto dispatch=[&](UINT level,ID3D12PipelineState* state,D3D12_GPU_DESCRIPTOR_HANDLE source,D3D12_GPU_DESCRIPTOR_HANDLE destination)
    {
        LevelParameters parameters{level?widths[level-1]:0,level?heights[level-1]:0,widths[level],heights[level]};
        commands.SetComputeRoot32BitConstants(0,4,&parameters,0);
        commands.SetComputeRootDescriptorTable(2,source);
        commands.SetComputeRootDescriptorTable(3,destination);
        commands.SetPipelineState(state);
        commands.Dispatch((widths[level]+7)/8,(heights[level]+7)/8,1);
    };
    dispatch(0,copy_.Get(),output.levels[0],output.levels[0]);
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery+1);
    result=states.UavBarrier(commands,*output.texture.Get(),"HiZ.CopyToReduce");
    if(FAILED(result))return result;
    for(UINT level=1;level<count;++level)
    {
        dispatch(level,reduce_.Get(),output.levels[level-1],output.levels[level]);
        result=states.UavBarrier(commands,*output.texture.Get(),"HiZ.Level");
        if(FAILED(result))return result;
    }
    if(timestamps.heap)commands.EndQuery(timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,firstQuery+2);
    result=states.Use(commands,*output.texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"HiZ.Read");
    return result;
}
