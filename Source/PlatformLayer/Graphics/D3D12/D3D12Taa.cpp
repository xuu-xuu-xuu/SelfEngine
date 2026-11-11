#include "D3D12Taa.h"
#include "D3D12Mesh.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include "D3D12Shader.h"
#include "D3D12StateTracker.h"
#include "CoreLayer/Localization.h"
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace
{
// 固定槽位；解析需要 [color,velocity,depth,historyColor,historyDepth] 五张连续 SRV，
// 因此为两组 historical index 各保留一份表头。
enum : UINT
{
    SlotColor=0,SlotVelocity=1,SlotDepth=2,SlotHistoryColor0=3,SlotHistoryDepth0=4,
    SlotColorB=5,SlotVelocityB=6,SlotDepthB=7,SlotHistoryColor1=8,SlotHistoryDepth1=9,
    SlotHistoryColorUav0=10,SlotHistoryDepthUav0=11,SlotHistoryColorUav1=12,SlotHistoryDepthUav1=13,
    SlotCount=14,
};
D3D12_CPU_DESCRIPTOR_HANDLE CpuAt(D3D12_CPU_DESCRIPTOR_HANDLE handle,UINT increment,UINT index)
{
    handle.ptr+=static_cast<SIZE_T>(increment)*index;return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE GpuAt(D3D12_GPU_DESCRIPTOR_HANDLE handle,UINT increment,UINT index)
{
    handle.ptr+=static_cast<UINT64>(increment)*index;return handle;
}
bool ValidColor(const D3D12_RESOURCE_DESC& description,UINT width,UINT height)
{
    return description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&description.Width==width
        &&description.Height==height&&description.DepthOrArraySize==1&&description.MipLevels==1
        &&description.SampleDesc.Count==1&&description.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
}
bool ValidDepth(const D3D12_RESOURCE_DESC& description,UINT width,UINT height)
{
    return description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&description.Width==width
        &&description.Height==height&&description.DepthOrArraySize==1&&description.MipLevels==1
        &&description.SampleDesc.Count==1
        &&(description.Format==DXGI_FORMAT_D32_FLOAT||description.Format==DXGI_FORMAT_R32_FLOAT);
}
struct VelocityConstants
{
    se::Mat4 currentClipFromLocal,previousClipFromLocal;
};
static_assert(sizeof(VelocityConstants)==128);
}
HRESULT D3D12Taa::Initialize(ID3D12Device& device,D3D12MemoryAllocator& memory,
    const std::filesystem::path& shaders,const Localization& text)
{
    if(device_)return E_UNEXPECTED;
    ComPtr<ID3DBlob> vertex,pixel,resolveCode;
    if(!CompileD3D12Shader(shaders/"TaaVelocity.hlsl","VSMain","vs_5_1",vertex,text,D3DCOMPILE_IEEE_STRICTNESS)
        ||!CompileD3D12Shader(shaders/"TaaVelocity.hlsl","PSMain","ps_5_1",pixel,text,D3DCOMPILE_IEEE_STRICTNESS)
        ||!CompileD3D12Shader(shaders/"TaaResolve.hlsl","Resolve","cs_5_1",resolveCode,text,D3DCOMPILE_IEEE_STRICTNESS))
    {CheckGraphicsResult(E_FAIL,text,"D3D12.TaaShaderFailed");return E_FAIL;}
    D3D12_ROOT_PARAMETER velocityParameter{};
    velocityParameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    velocityParameter.Constants={0,0,32};velocityParameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC velocityDescription{};
    velocityDescription.NumParameters=1;velocityDescription.pParameters=&velocityParameter;
    velocityDescription.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> velocitySignature,error;
    auto result=D3D12SerializeRootSignature(&velocityDescription,D3D_ROOT_SIGNATURE_VERSION_1,velocitySignature.GetAddressOf(),error.GetAddressOf());
    ComPtr<ID3D12RootSignature> velocityRoot;
    if(SUCCEEDED(result))result=device.CreateRootSignature(0,velocitySignature->GetBufferPointer(),velocitySignature->GetBufferSize(),
        IID_PPV_ARGS(velocityRoot.GetAddressOf()));
    // 解析根签名：8 个根常量、5 张 SRV、2 张 UAV、一个线性 clamp 采样器。
    D3D12_DESCRIPTOR_RANGE srvRange{},uavRange{};
    srvRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;srvRange.NumDescriptors=5;
    srvRange.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    uavRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;uavRange.NumDescriptors=2;
    uavRange.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER resolveParameters[3]{};
    resolveParameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    resolveParameters[0].Constants={0,0,8};resolveParameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    resolveParameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolveParameters[1].DescriptorTable={1,&srvRange};resolveParameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    resolveParameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolveParameters[2].DescriptorTable={1,&uavRange};resolveParameters[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC resolveDescription{};
    resolveDescription.NumParameters=3;resolveDescription.pParameters=resolveParameters;
    resolveDescription.NumStaticSamplers=1;resolveDescription.pStaticSamplers=&sampler;
    resolveDescription.Flags=D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> resolveSignature;
    if(SUCCEEDED(result))result=D3D12SerializeRootSignature(&resolveDescription,D3D_ROOT_SIGNATURE_VERSION_1,resolveSignature.GetAddressOf(),error.GetAddressOf());
    ComPtr<ID3D12RootSignature> resolveRoot;
    if(SUCCEEDED(result))result=device.CreateRootSignature(0,resolveSignature->GetBufferPointer(),resolveSignature->GetBufferSize(),
        IID_PPV_ARGS(resolveRoot.GetAddressOf()));
    const D3D12_INPUT_ELEMENT_DESC legacyInput{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    const D3D12_INPUT_ELEMENT_DESC compressedInput{"POSITION",0,DXGI_FORMAT_R16G16B16A16_UNORM,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    ComPtr<ID3D12PipelineState> legacy,compressed;
    const auto createVelocity=[&](const D3D12_INPUT_ELEMENT_DESC& input,ComPtr<ID3D12PipelineState>& outputState,const wchar_t* name){
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature=velocityRoot.Get();
        description.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};
        description.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
        description.InputLayout={&input,1};description.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets=1;description.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;description.DSVFormat=DXGI_FORMAT_D32_FLOAT;
        description.SampleDesc={1,0};description.SampleMask=UINT_MAX;
        description.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;description.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        description.RasterizerState.DepthClipEnable=TRUE;description.RasterizerState.FrontCounterClockwise=TRUE;
        description.DepthStencilState.DepthEnable=TRUE;description.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        description.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        description.DepthStencilState.FrontFace=description.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
        description.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        const auto created=device.CreateGraphicsPipelineState(&description,IID_PPV_ARGS(outputState.GetAddressOf()));
        if(SUCCEEDED(created))outputState->SetName(name);return created;
    };
    if(SUCCEEDED(result))result=createVelocity(legacyInput,legacy,L"SelfEngine.TaaVelocityLegacy");
    if(SUCCEEDED(result))result=createVelocity(compressedInput,compressed,L"SelfEngine.TaaVelocityCompressed");
    ComPtr<ID3D12PipelineState> resolveState;
    if(SUCCEEDED(result))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature=resolveRoot.Get();pipeline.CS={resolveCode->GetBufferPointer(),resolveCode->GetBufferSize()};
        result=device.CreateComputePipelineState(&pipeline,IID_PPV_ARGS(resolveState.GetAddressOf()));
        if(SUCCEEDED(result))resolveState->SetName(L"SelfEngine.TaaResolve");
    }
    if(FAILED(result)){CheckGraphicsResult(result,text,"D3D12.TaaShaderFailed");return result;}
    velocityRoot->SetName(L"SelfEngine.TaaVelocityRoot");resolveRoot->SetName(L"SelfEngine.TaaResolveRoot");
    device_=&device;memory_=&memory;velocityRoot_=std::move(velocityRoot);resolveRoot_=std::move(resolveRoot);
    velocityLegacy_=std::move(legacy);velocityCompressed_=std::move(compressed);resolve_=std::move(resolveState);return S_OK;
}
HRESULT D3D12Taa::CreateOutput(ID3D12Resource& hdr,ID3D12Resource& depth,UINT width,UINT height,D3D12TaaOutput& output) const
{
    if(!Ready()||!memory_)return E_UNEXPECTED;
    if(!width||!height||width>8192||height>8192||!ValidColor(hdr.GetDesc(),width,height)||!ValidDepth(depth.GetDesc(),width,height))
        return E_INVALIDARG;
    D3D12TaaOutput candidate;candidate.width=width;candidate.height=height;
    D3D12_RESOURCE_DESC velocity{};
    velocity.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;velocity.Width=width;velocity.Height=height;
    velocity.DepthOrArraySize=velocity.MipLevels=1;velocity.Format=DXGI_FORMAT_R16G16_FLOAT;velocity.SampleDesc={1,0};
    velocity.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE velocityClear{};velocityClear.Format=DXGI_FORMAT_R16G16_FLOAT;
    velocityClear.Color[0]=velocityClear.Color[1]=velocityClear.Color[2]=velocityClear.Color[3]=0;
    auto result=memory_->Create(velocity,D3D12_RESOURCE_STATE_COMMON,&velocityClear,candidate.velocity,"Taa.Velocity");
    if(FAILED(result))return result;
    D3D12_RESOURCE_DESC color{};
    color.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;color.Width=width;color.Height=height;
    color.DepthOrArraySize=color.MipLevels=1;color.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;color.SampleDesc={1,0};
    color.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC depthHistory{};
    depthHistory.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;depthHistory.Width=width;depthHistory.Height=height;
    depthHistory.DepthOrArraySize=depthHistory.MipLevels=1;depthHistory.Format=DXGI_FORMAT_R32_FLOAT;depthHistory.SampleDesc={1,0};
    depthHistory.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for(UINT index=0;index<2;++index)
    {
        result=memory_->Create(color,D3D12_RESOURCE_STATE_COMMON,nullptr,candidate.historyColor[index],
            index?"Taa.HistoryColorB":"Taa.HistoryColorA");
        if(FAILED(result))return result;
        result=memory_->Create(depthHistory,D3D12_RESOURCE_STATE_COMMON,nullptr,candidate.historyDepth[index],
            index?"Taa.HistoryDepthB":"Taa.HistoryDepthA");
        if(FAILED(result))return result;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
    rtvHeap.NumDescriptors=1;rtvHeap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtvHeap.NodeMask=1;
    result=device_->CreateDescriptorHeap(&rtvHeap,IID_PPV_ARGS(candidate.rtvs.GetAddressOf()));
    if(FAILED(result))return result;
    candidate.velocityRtv=candidate.rtvs->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(candidate.velocity.Get(),nullptr,candidate.velocityRtv);
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeap{};
    dsvHeap.NumDescriptors=1;dsvHeap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dsvHeap.NodeMask=1;
    result=device_->CreateDescriptorHeap(&dsvHeap,IID_PPV_ARGS(candidate.dsvs.GetAddressOf()));
    if(FAILED(result))return result;
    candidate.depthDsv=candidate.dsvs->GetCPUDescriptorHandleForHeapStart();
    device_->CreateDepthStencilView(&depth,nullptr,candidate.depthDsv);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.NumDescriptors=descriptorCount;heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;heap.NodeMask=1;
    result=device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(candidate.descriptors.GetAddressOf()));
    if(FAILED(result))return result;
    candidate.descriptors->SetName(L"SelfEngine.TaaDescriptors");
    const auto increment=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu=candidate.descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto gpu=candidate.descriptors->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrv{};
    colorSrv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;colorSrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    colorSrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;colorSrv.Texture2D={0,1,0,0};
    device_->CreateShaderResourceView(&hdr,&colorSrv,CpuAt(cpu,increment,SlotColor));
    device_->CreateShaderResourceView(&hdr,&colorSrv,CpuAt(cpu,increment,SlotColorB));
    D3D12_SHADER_RESOURCE_VIEW_DESC velocitySrv{};
    velocitySrv.Format=DXGI_FORMAT_R16G16_FLOAT;velocitySrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    velocitySrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;velocitySrv.Texture2D={0,1,0,0};
    device_->CreateShaderResourceView(candidate.velocity.Get(),&velocitySrv,CpuAt(cpu,increment,SlotVelocity));
    device_->CreateShaderResourceView(candidate.velocity.Get(),&velocitySrv,CpuAt(cpu,increment,SlotVelocityB));
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Format=DXGI_FORMAT_R32_FLOAT;depthSrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;depthSrv.Texture2D={0,1,0,0};
    device_->CreateShaderResourceView(&depth,&depthSrv,CpuAt(cpu,increment,SlotDepth));
    device_->CreateShaderResourceView(&depth,&depthSrv,CpuAt(cpu,increment,SlotDepthB));
    D3D12_SHADER_RESOURCE_VIEW_DESC historyColorSrv{colorSrv},historyDepthSrv{depthSrv};
    D3D12_UNORDERED_ACCESS_VIEW_DESC historyColorUav{};
    historyColorUav.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;historyColorUav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    historyColorUav.Texture2D={0,0};
    D3D12_UNORDERED_ACCESS_VIEW_DESC historyDepthUav{};
    historyDepthUav.Format=DXGI_FORMAT_R32_FLOAT;historyDepthUav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    historyDepthUav.Texture2D={0,0};
    for(UINT index=0;index<2;++index)
    {
        device_->CreateShaderResourceView(candidate.historyColor[index].Get(),&historyColorSrv,
            CpuAt(cpu,increment,index?SlotHistoryColor1:SlotHistoryColor0));
        device_->CreateShaderResourceView(candidate.historyDepth[index].Get(),&historyDepthSrv,
            CpuAt(cpu,increment,index?SlotHistoryDepth1:SlotHistoryDepth0));
        device_->CreateUnorderedAccessView(candidate.historyColor[index].Get(),nullptr,&historyColorUav,
            CpuAt(cpu,increment,index?SlotHistoryColorUav1:SlotHistoryColorUav0));
        device_->CreateUnorderedAccessView(candidate.historyDepth[index].Get(),nullptr,&historyDepthUav,
            CpuAt(cpu,increment,index?SlotHistoryDepthUav1:SlotHistoryDepthUav0));
    }
    candidate.colorSrv=GpuAt(gpu,increment,SlotColor);candidate.velocitySrv=GpuAt(gpu,increment,SlotVelocity);
    candidate.depthSrv=GpuAt(gpu,increment,SlotDepth);
    candidate.historyColorSrv[0]=GpuAt(gpu,increment,SlotHistoryColor0);candidate.historyColorSrv[1]=GpuAt(gpu,increment,SlotHistoryColor1);
    candidate.historyDepthSrv[0]=GpuAt(gpu,increment,SlotHistoryDepth0);candidate.historyDepthSrv[1]=GpuAt(gpu,increment,SlotHistoryDepth1);
    candidate.historyColorUav[0]=GpuAt(gpu,increment,SlotHistoryColorUav0);candidate.historyColorUav[1]=GpuAt(gpu,increment,SlotHistoryColorUav1);
    candidate.historyDepthUav[0]=GpuAt(gpu,increment,SlotHistoryDepthUav0);candidate.historyDepthUav[1]=GpuAt(gpu,increment,SlotHistoryDepthUav1);
    candidate.srvTable[0]=GpuAt(gpu,increment,SlotColor);candidate.srvTable[1]=GpuAt(gpu,increment,SlotColorB);
    candidate.uavTable[0]=GpuAt(gpu,increment,SlotHistoryColorUav0);candidate.uavTable[1]=GpuAt(gpu,increment,SlotHistoryColorUav1);
    output=std::move(candidate);return S_OK;
}
HRESULT D3D12Taa::RecordVelocity(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,D3D12TaaOutput& output,
    std::span<const D3D12MeshDraw> draws,const se::Mat4& clipFromWorld,ID3D12Resource& depth) const
{
    if(!Ready())return E_UNEXPECTED;
    if(!output.velocity||!output.rtvs||!output.dsvs||!output.width||!output.height
        ||!ValidDepth(depth.GetDesc(),output.width,output.height)
        ||!se::IsFinite(clipFromWorld))return E_INVALIDARG;
    HRESULT result=states.Use(commands,depth,D3D12_RESOURCE_STATE_DEPTH_READ,"Taa.DepthRead");
    if(SUCCEEDED(result))result=states.Use(commands,*output.velocity.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"Taa.Velocity");
    if(FAILED(result))return result;
    const float zero[4]{0,0,0,0};
    commands.ClearRenderTargetView(output.velocityRtv,zero,0,nullptr);
    commands.OMSetRenderTargets(1,&output.velocityRtv,FALSE,&output.depthDsv);
    const D3D12_VIEWPORT viewport{0,0,float(output.width),float(output.height),0,1};
    const D3D12_RECT scissor{0,0,LONG(output.width),LONG(output.height)};
    commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);
    commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands.SetGraphicsRootSignature(velocityRoot_.Get());
    for(const auto& draw:draws)
    {
        if(!draw.mesh||!draw.mesh->indexCount||!draw.instances.empty())continue;
        const auto& previous=draw.hasPreviousWorld?draw.previousWorld:draw.world;
        if(!se::IsFinite(draw.world)||!se::IsFinite(previous)||!se::IsFinite(clipFromWorld*draw.world)
            ||!se::IsFinite(clipFromWorld*previous))continue;
        const VelocityConstants constants{clipFromWorld*draw.world,clipFromWorld*previous};
        commands.SetPipelineState((draw.compressedVertices?velocityCompressed_:velocityLegacy_).Get());
        commands.SetGraphicsRoot32BitConstants(0,32,&constants,0);
        commands.IASetVertexBuffers(0,1,&draw.mesh->vertexView);
        commands.IASetIndexBuffer(&draw.mesh->indexView);
        commands.DrawIndexedInstanced(draw.mesh->indexCount,1,0,0,0);
    }
    return S_OK;
}
HRESULT D3D12Taa::Resolve(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,D3D12TaaOutput& output,
    ID3D12Resource& currentColor,ID3D12Resource& depth,const D3D12TaaParameters& parameters) const
{
    if(!Ready())return E_UNEXPECTED;
    if(!output.velocity||!output.historyColor[0]||!output.historyColor[1]||!output.historyDepth[0]||!output.historyDepth[1]
        ||!output.descriptors||!output.width||!output.height
        ||!ValidColor(currentColor.GetDesc(),output.width,output.height)||!ValidDepth(depth.GetDesc(),output.width,output.height)
        ||!std::isfinite(parameters.staticBlend)||!std::isfinite(parameters.dynamicBlend)
        ||parameters.staticBlend<0||parameters.staticBlend>1||parameters.dynamicBlend<0||parameters.dynamicBlend>1)return E_INVALIDARG;
    const UINT previous=output.historyIndex,current=1-previous;
    HRESULT result=states.Use(commands,*output.velocity.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Taa.VelocityRead");
    if(SUCCEEDED(result))result=states.Use(commands,currentColor,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Taa.ColorRead");
    if(SUCCEEDED(result))result=states.Use(commands,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Taa.DepthSample");
    if(SUCCEEDED(result))result=states.Use(commands,*output.historyColor[previous].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Taa.HistoryRead");
    if(SUCCEEDED(result))result=states.Use(commands,*output.historyDepth[previous].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Taa.HistoryDepthRead");
    if(SUCCEEDED(result))result=states.Use(commands,*output.historyColor[current].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Taa.HistoryWrite");
    if(SUCCEEDED(result))result=states.Use(commands,*output.historyDepth[current].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Taa.HistoryDepthWrite");
    if(FAILED(result))return result;
    auto constants=parameters;constants.width=float(output.width);constants.height=float(output.height);
    ID3D12DescriptorHeap* heaps[]{output.descriptors.Get()};
    commands.SetDescriptorHeaps(1,heaps);
    commands.SetComputeRootSignature(resolveRoot_.Get());
    commands.SetComputeRoot32BitConstants(0,8,&constants,0);
    commands.SetComputeRootDescriptorTable(1,output.srvTable[previous]);
    commands.SetComputeRootDescriptorTable(2,output.uavTable[current]);
    commands.SetPipelineState(resolve_.Get());
    commands.Dispatch((output.width+7)/8,(output.height+7)/8,1);
    result=states.UavBarrier(commands,*output.historyColor[current].Get(),"Taa.HistoryBarrier");
    if(SUCCEEDED(result))result=states.UavBarrier(commands,*output.historyDepth[current].Get(),"Taa.HistoryDepthBarrier");
    if(SUCCEEDED(result))result=states.Use(commands,*output.historyColor[current].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Taa.HistorySample");
    if(SUCCEEDED(result))result=states.Use(commands,*output.historyDepth[current].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Taa.HistoryDepthSample");
    if(FAILED(result))return result;
    output.historyIndex=current;++output.frames;return S_OK;
}
