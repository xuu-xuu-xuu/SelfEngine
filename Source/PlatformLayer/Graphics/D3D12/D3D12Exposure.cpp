#include "D3D12Exposure.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include "D3D12Shader.h"
#include "D3D12StateTracker.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Math/Half.h"
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace
{
// 固定描述符槽位；根签名表宽固定为 SRV 2、UAV 1。
// 适应需要 [histogram, previousExposure] 两张连续 SRV，因此为两张曝光纹理各配一份
// 直方图 SRV：槽 1/2 = hist+exp0，槽 3/4 = hist+exp1。
enum : UINT
{
    SlotHdrSrv=0,SlotHistogramSrvA=1,SlotExposureSrv0=2,SlotHistogramSrvB=3,SlotExposureSrv1=4,
    SlotHistogramUav=5,SlotExposureUav0=6,SlotExposureUav1=7,SlotLutUav=8,SlotLutSrv=9,
};
D3D12_CPU_DESCRIPTOR_HANDLE CpuAt(D3D12_CPU_DESCRIPTOR_HANDLE handle,UINT increment,UINT index)
{
    handle.ptr+=static_cast<SIZE_T>(increment)*index;return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE GpuAt(D3D12_GPU_DESCRIPTOR_HANDLE handle,UINT increment,UINT index)
{
    handle.ptr+=static_cast<UINT64>(increment)*index;return handle;
}
bool ValidSource(const D3D12_RESOURCE_DESC& description,UINT width,UINT height)
{
    return description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&description.Width==width
        &&description.Height==height&&description.DepthOrArraySize==1&&description.MipLevels==1
        &&description.SampleDesc.Count==1&&description.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
}
}
HRESULT D3D12Exposure::Initialize(ID3D12Device& device,D3D12MemoryAllocator& memory,
    const std::filesystem::path& shaders,const Localization& text)
{
    if(device_)return E_UNEXPECTED;
    ComPtr<ID3DBlob> clear,histogram,adapt,lut;
    if(!CompileD3D12Shader(shaders/"ExposureHistogram.hlsl","ClearHistogram","cs_5_1",clear,text,D3DCOMPILE_IEEE_STRICTNESS)
        ||!CompileD3D12Shader(shaders/"ExposureHistogram.hlsl","BuildHistogram","cs_5_1",histogram,text,D3DCOMPILE_IEEE_STRICTNESS)
        ||!CompileD3D12Shader(shaders/"ExposureAdapt.hlsl","AdaptExposure","cs_5_1",adapt,text,D3DCOMPILE_IEEE_STRICTNESS)
        ||!CompileD3D12Shader(shaders/"ExposureLut.hlsl","BuildLut","cs_5_1",lut,text,D3DCOMPILE_IEEE_STRICTNESS))
    {CheckGraphicsResult(E_FAIL,text,"D3D12.ExposureShaderFailed");return E_FAIL;}
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=2;
    ranges[0].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=1;
    ranges[1].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,24};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={1,&ranges[0]};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable={1,&ranges[1]};parameters[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters=3;description.pParameters=parameters;description.Flags=D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> signature,error;
    auto result=D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,signature.GetAddressOf(),error.GetAddressOf());
    ComPtr<ID3D12RootSignature> root;
    if(SUCCEEDED(result))result=device.CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),
        IID_PPV_ARGS(root.GetAddressOf()));
    ComPtr<ID3D12PipelineState> clearState,histogramState,adaptState,lutState;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature=root.Get();
    if(SUCCEEDED(result))
    {
        pipeline.CS={clear->GetBufferPointer(),clear->GetBufferSize()};
        result=device.CreateComputePipelineState(&pipeline,IID_PPV_ARGS(clearState.GetAddressOf()));
    }
    if(SUCCEEDED(result))
    {
        pipeline.CS={histogram->GetBufferPointer(),histogram->GetBufferSize()};
        result=device.CreateComputePipelineState(&pipeline,IID_PPV_ARGS(histogramState.GetAddressOf()));
    }
    if(SUCCEEDED(result))
    {
        pipeline.CS={adapt->GetBufferPointer(),adapt->GetBufferSize()};
        result=device.CreateComputePipelineState(&pipeline,IID_PPV_ARGS(adaptState.GetAddressOf()));
    }
    if(SUCCEEDED(result))
    {
        pipeline.CS={lut->GetBufferPointer(),lut->GetBufferSize()};
        result=device.CreateComputePipelineState(&pipeline,IID_PPV_ARGS(lutState.GetAddressOf()));
    }
    if(FAILED(result)){CheckGraphicsResult(result,text,"D3D12.ExposureShaderFailed");return result;}
    root->SetName(L"SelfEngine.ExposureRootSignature");
    clearState->SetName(L"SelfEngine.ExposureClear");histogramState->SetName(L"SelfEngine.ExposureHistogram");
    adaptState->SetName(L"SelfEngine.ExposureAdapt");lutState->SetName(L"SelfEngine.ExposureLut");
    device_=&device;memory_=&memory;root_=std::move(root);clear_=std::move(clearState);
    histogram_=std::move(histogramState);adapt_=std::move(adaptState);lut_=std::move(lutState);return S_OK;
}
HRESULT D3D12Exposure::CreateOutput(ID3D12Resource& hdr,UINT width,UINT height,D3D12ExposureOutput& output) const
{
    if(!Ready()||!memory_)return E_UNEXPECTED;
    if(!width||!height||width>8192||height>8192||!ValidSource(hdr.GetDesc(),width,height))return E_INVALIDARG;
    D3D12ExposureOutput candidate;candidate.width=width;candidate.height=height;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=UINT64(histogramBins)*sizeof(std::uint32_t);
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc={1,0};
    buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto result=memory_->Create(buffer,D3D12_RESOURCE_STATE_COMMON,nullptr,candidate.histogram,"Exposure.Histogram");
    if(FAILED(result))return result;
    D3D12_RESOURCE_DESC exposure{};
    exposure.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;exposure.Width=exposure.Height=1;
    exposure.DepthOrArraySize=exposure.MipLevels=1;exposure.Format=DXGI_FORMAT_R32_FLOAT;exposure.SampleDesc={1,0};
    exposure.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for(UINT index=0;index<2;++index)
    {
        result=memory_->Create(exposure,D3D12_RESOURCE_STATE_COMMON,nullptr,candidate.exposure[index],
            index?"Exposure.AdaptedB":"Exposure.AdaptedA");
        if(FAILED(result))return result;
    }
    D3D12_RESOURCE_DESC lut{};
    lut.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;lut.Width=lut.Height=lut.DepthOrArraySize=lutSize;
    lut.MipLevels=1;lut.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;lut.SampleDesc={1,0};
    lut.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    result=memory_->Create(lut,D3D12_RESOURCE_STATE_COMMON,nullptr,candidate.lut,"Exposure.Lut");
    if(FAILED(result))return result;
    // `.selut` 上传缓冲：33 行 × 33 层，行 pitch 按 D3D12 纹理复制对齐（33×8=264 → 512）。
    constexpr UINT lutRowPitch=512;
    D3D12_RESOURCE_DESC lutUpload{};
    lutUpload.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    lutUpload.Width=UINT64{lutRowPitch}*lutSize*lutSize;
    lutUpload.Height=lutUpload.DepthOrArraySize=lutUpload.MipLevels=1;lutUpload.SampleDesc={1,0};
    lutUpload.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type=D3D12_HEAP_TYPE_UPLOAD;uploadHeap.CreationNodeMask=uploadHeap.VisibleNodeMask=1;
    result=device_->CreateCommittedResource(&uploadHeap,D3D12_HEAP_FLAG_NONE,&lutUpload,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(candidate.lutUpload.GetAddressOf()));
    if(FAILED(result))return result;
    candidate.lutUpload->SetName(L"SelfEngine.ExposureLutUpload");
    candidate.lutUploadRowPitch=lutRowPitch;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.NumDescriptors=descriptorCount;heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;heap.NodeMask=1;
    result=device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(candidate.descriptors.GetAddressOf()));
    if(FAILED(result))return result;
    candidate.descriptors->SetName(L"SelfEngine.ExposureDescriptors");
    const auto increment=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu=candidate.descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto gpu=candidate.descriptors->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D={0,1,0,0};
    device_->CreateShaderResourceView(&hdr,&srv,CpuAt(cpu,increment,SlotHdrSrv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format=DXGI_FORMAT_R32_UINT;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer={0,histogramBins,0,0};
    device_->CreateUnorderedAccessView(candidate.histogram.Get(),nullptr,&uav,CpuAt(cpu,increment,SlotHistogramUav));
    D3D12_SHADER_RESOURCE_VIEW_DESC histogramSrv{};
    histogramSrv.Format=DXGI_FORMAT_R32_UINT;histogramSrv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    histogramSrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    histogramSrv.Buffer={0,histogramBins,0};
    device_->CreateShaderResourceView(candidate.histogram.Get(),&histogramSrv,CpuAt(cpu,increment,SlotHistogramSrvA));
    device_->CreateShaderResourceView(candidate.histogram.Get(),&histogramSrv,CpuAt(cpu,increment,SlotHistogramSrvB));
    D3D12_SHADER_RESOURCE_VIEW_DESC exposureSrv{};
    exposureSrv.Format=DXGI_FORMAT_R32_FLOAT;exposureSrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    exposureSrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;exposureSrv.Texture2D={0,1,0,0};
    D3D12_UNORDERED_ACCESS_VIEW_DESC exposureUav{};
    exposureUav.Format=DXGI_FORMAT_R32_FLOAT;exposureUav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    exposureUav.Texture2D={0,0};
    for(UINT index=0;index<2;++index)
    {
        const UINT srvSlot=index?SlotExposureSrv1:SlotExposureSrv0;
        const UINT uavSlot=index?SlotExposureUav1:SlotExposureUav0;
        device_->CreateShaderResourceView(candidate.exposure[index].Get(),&exposureSrv,CpuAt(cpu,increment,srvSlot));
        device_->CreateUnorderedAccessView(candidate.exposure[index].Get(),nullptr,&exposureUav,CpuAt(cpu,increment,uavSlot));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC lutSrv{};
    lutSrv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;lutSrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;
    lutSrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;lutSrv.Texture3D={0,1,0};
    device_->CreateShaderResourceView(candidate.lut.Get(),&lutSrv,CpuAt(cpu,increment,SlotLutSrv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC lutUav{};
    lutUav.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;lutUav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;
    lutUav.Texture3D={0,0,lutSize};
    device_->CreateUnorderedAccessView(candidate.lut.Get(),nullptr,&lutUav,CpuAt(cpu,increment,SlotLutUav));
    candidate.hdrSrv=GpuAt(gpu,increment,SlotHdrSrv);
    candidate.histogramSrvA=GpuAt(gpu,increment,SlotHistogramSrvA);candidate.histogramSrvB=GpuAt(gpu,increment,SlotHistogramSrvB);
    candidate.exposureSrv[0]=GpuAt(gpu,increment,SlotExposureSrv0);candidate.exposureSrv[1]=GpuAt(gpu,increment,SlotExposureSrv1);
    candidate.histogramUav=GpuAt(gpu,increment,SlotHistogramUav);
    candidate.exposureUav[0]=GpuAt(gpu,increment,SlotExposureUav0);candidate.exposureUav[1]=GpuAt(gpu,increment,SlotExposureUav1);
    candidate.lutUav=GpuAt(gpu,increment,SlotLutUav);candidate.lutSrv=GpuAt(gpu,increment,SlotLutSrv);
    output=std::move(candidate);return S_OK;
}
HRESULT D3D12Exposure::Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
    D3D12ExposureOutput& output,const D3D12ExposureParameters& parameters,bool exposureEnabled,bool initialize,bool rebuildLut) const
{
    if(!Ready())return E_UNEXPECTED;
    if(!output.histogram||!output.exposure[0]||!output.exposure[1]||!output.lut||!output.descriptors
        ||!output.width||!output.height||output.width>8192||output.height>8192)return E_INVALIDARG;
    if(!exposureEnabled&&!rebuildLut)return S_OK;
    auto constants=parameters;constants.adaptation[2]=initialize?1.0f:0.0f;
    const UINT previous=output.exposureIndex,current=1-previous;
    ID3D12DescriptorHeap* heaps[]{output.descriptors.Get()};
    commands.SetDescriptorHeaps(1,heaps);
    commands.SetComputeRootSignature(root_.Get());
    commands.SetComputeRoot32BitConstants(0,24,&constants,0);
    const auto dispatch=[&](ID3D12PipelineState* state,D3D12_GPU_DESCRIPTOR_HANDLE source,D3D12_GPU_DESCRIPTOR_HANDLE destination,UINT x,UINT y,UINT z)
    {
        commands.SetComputeRootDescriptorTable(1,source);commands.SetComputeRootDescriptorTable(2,destination);
        commands.SetPipelineState(state);commands.Dispatch(x,y,z);
    };
    HRESULT result=S_OK;
    if(exposureEnabled)
    {
        result=states.Use(commands,*output.histogram.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Exposure.Histogram");
        if(SUCCEEDED(result))result=states.Use(commands,*output.exposure[current].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Exposure.Adapt");
        if(SUCCEEDED(result))result=states.Use(commands,*output.exposure[previous].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Exposure.History");
        if(FAILED(result))return result;
        dispatch(clear_.Get(),output.hdrSrv,output.histogramUav,1,1,1);
        result=states.UavBarrier(commands,*output.histogram.Get(),"Exposure.ClearToBuild");
        if(FAILED(result))return result;
        dispatch(histogram_.Get(),output.hdrSrv,output.histogramUav,(output.width+63)/64,(output.height+63)/64,1);
        result=states.UavBarrier(commands,*output.histogram.Get(),"Exposure.BuildToAdapt");
        if(FAILED(result))return result;
        result=states.Use(commands,*output.histogram.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Exposure.HistogramRead");
        if(FAILED(result))return result;
        dispatch(adapt_.Get(),previous?output.histogramSrvB:output.histogramSrvA,
            current?output.exposureUav[1]:output.exposureUav[0],1,1,1);
        result=states.UavBarrier(commands,*output.exposure[current].Get(),"Exposure.AdaptToSample");
        if(FAILED(result))return result;
        result=states.Use(commands,*output.exposure[current].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Exposure.Sample");
        if(FAILED(result))return result;
    }
    if(rebuildLut)
    {
        result=states.Use(commands,*output.lut.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Exposure.LutBuild");
        if(FAILED(result))return result;
        dispatch(lut_.Get(),output.hdrSrv,output.lutUav,(lutSize+3)/4,(lutSize+3)/4,(lutSize+3)/4);
        result=states.UavBarrier(commands,*output.lut.Get(),"Exposure.LutToSample");
        if(FAILED(result))return result;
        result=states.Use(commands,*output.lut.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Exposure.LutSample");
        if(FAILED(result))return result;
    }
    if(exposureEnabled){output.exposureIndex=current;++output.frames;}
    return S_OK;
}
HRESULT D3D12Exposure::UploadLut(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
    D3D12ExposureOutput& output,std::span<const se::Vec3> values) const
{
    if(!Ready())return E_UNEXPECTED;
    if(!output.lut||!output.lutUpload||!output.lutUploadRowPitch||output.lutUploadRowPitch%256)return E_INVALIDARG;
    if(values.size()!=std::size_t{lutSize}*lutSize*lutSize)return E_INVALIDARG;
    // 16F 上界与 `.selut` 读取契约一致；非有限/负值在记录前拒绝，不污染采样纹理。
    for(const auto& value:values)
        if(!std::isfinite(value.x)||!std::isfinite(value.y)||!std::isfinite(value.z)
            ||value.x<0||value.x>65504||value.y<0||value.y>65504||value.z<0||value.z>65504)return E_INVALIDARG;
    auto result=states.Use(commands,*output.lut.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Exposure.LutUpload");
    if(FAILED(result))return result;
    void* mapped=nullptr;const D3D12_RANGE readRange{0,0};
    if(FAILED(output.lutUpload->Map(0,&readRange,&mapped))||!mapped)return E_FAIL;
    const UINT pitch=output.lutUploadRowPitch;
    const UINT64 bytes=UINT64{pitch}*lutSize*lutSize;
    std::memset(mapped,0,static_cast<std::size_t>(bytes));
    // CPU 晶格索引为 (r*size+g)*size+b；D3D 3D 纹理的 x/y/z 依次是 r/g/b。
    for(std::uint32_t blue=0;blue<lutSize;++blue)
        for(std::uint32_t green=0;green<lutSize;++green)
            for(std::uint32_t red=0;red<lutSize;++red)
            {
                const auto& value=values[(std::size_t{red}*lutSize+green)*lutSize+blue];
                const std::uint16_t texel[4]{se::FloatToHalf(value.x),se::FloatToHalf(value.y),
                    se::FloatToHalf(value.z),se::FloatToHalf(1.0f)};
                auto* destination=static_cast<std::byte*>(mapped)
                    +(UINT64{blue}*lutSize+green)*pitch+UINT64{red}*8;
                std::memcpy(destination,texel,sizeof(texel));
            }
    const D3D12_RANGE writtenRange{0,static_cast<SIZE_T>(bytes)};
    output.lutUpload->Unmap(0,&writtenRange);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource=output.lut.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex=0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource=output.lutUpload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,lutSize,lutSize,lutSize,pitch};
    commands.CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    return states.Use(commands,*output.lut.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Exposure.LutUpload");
}
