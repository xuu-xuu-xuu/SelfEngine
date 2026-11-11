#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include "CoreLayer/Math/Math.h"

class Localization;
class D3D12StateRecording;
class D3D12MemoryAllocator;

// G15：GPU 直方图、曝光适应与 33³ LUT 的常量。与 Exposure.hlsl 的 b0 逐字段对应；
// 通过根常量传递，避免每帧复制常量缓冲。
struct alignas(16) D3D12ExposureParameters
{
    std::array<float,4> percentiles{-12.0f,4.0f,0.60f,0.80f}; // minLog2,maxLog2,low,high
    std::array<float,4> range{0.18f,1.0f/1024.0f,64.0f,1.0f/60.0f}; // key,min,max,deltaSeconds
    std::array<float,4> adaptation{3.0f,1.0f,0.0f,0.0f}; // bright,dark,initialize(unused),reserved
    std::array<float,4> lutLift{0.0f,0.0f,0.0f,33.0f}; // rgb,size
    std::array<float,4> lutGamma{1.0f,1.0f,1.0f,0.0f};
    std::array<float,4> lutGain{1.0f,1.0f,1.0f,0.0f};
};
static_assert(sizeof(D3D12ExposureParameters)==96);

// 一次后处理目标组持有的曝光资源；曝光纹理 ping-pong，避免同一资源自读写。
struct D3D12ExposureOutput
{
    Microsoft::WRL::ComPtr<ID3D12Resource> histogram,exposure[2],lut;
    // G15：`.selut` 资产的暂存上传缓冲。与 LUT 同寿命，重载时只改内容，不每帧分配。
    Microsoft::WRL::ComPtr<ID3D12Resource> lutUpload;
    UINT lutUploadRowPitch=0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    D3D12_GPU_DESCRIPTOR_HANDLE hdrSrv{},histogramSrvA{},histogramSrvB{},
        exposureSrv[2]{},histogramUav{},exposureUav[2]{},lutUav{},lutSrv{};
    UINT width=0,height=0,exposureIndex=0;
    UINT64 frames=0;
};

// G15 GPU 曝光内核。直方图每帧只读一次 HDR；适应只写 1×1 曝光纹理；
// LUT 只在参数变化时重建。所有资源归输出组所有，resize 时整组重建。
class D3D12Exposure
{
public:
    static constexpr UINT lutSize=33,histogramBins=256,descriptorCount=10;
    HRESULT Initialize(ID3D12Device& device,D3D12MemoryAllocator& memory,
        const std::filesystem::path& shaders,const Localization& text);
    bool Ready() const noexcept {return device_&&root_&&clear_&&histogram_&&adapt_&&lut_;}
    // hdr 必须是单层、单 mip 的 R16G16B16A16_FLOAT；失败不替换 output。
    HRESULT CreateOutput(ID3D12Resource& hdr,UINT width,UINT height,D3D12ExposureOutput& output) const;
    // 记录 LUT（可选）与曝光链（可选）。成功时 exposureIndex 指向本帧
    // 已写入且处于 PIXEL_SHADER_RESOURCE 的曝光纹理，可直接供 ToneMap 采样。
    HRESULT Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,D3D12ExposureOutput& output,
        const D3D12ExposureParameters& parameters,bool exposureEnabled,bool initialize,bool rebuildLut) const;
    // G15：用 `.selut` 的显示线性晶格覆盖 33³ LUT（RGBA16F）。晶格索引与 CPU
    // ColorLut::Values 的 (r,g,b) 顺序一致；只在资产 revision 变化时调用。
    HRESULT UploadLut(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
        D3D12ExposureOutput& output,std::span<const se::Vec3> values) const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clear_,histogram_,adapt_,lut_;
    D3D12MemoryAllocator* memory_=nullptr;
};
