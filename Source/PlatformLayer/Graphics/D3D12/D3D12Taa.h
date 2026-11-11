#pragma once
#include "CoreLayer/Math/Projection.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

class Localization;
class D3D12StateRecording;
class D3D12MemoryAllocator;
struct D3D12MeshDraw;

// G14：RG16F 速度 + RGBA16F/R32F 历史 ping-pong。速度 Pass 只画不透明网格；
// 解析 Pass 在 GPU 上完成重投影、YCoCg 邻域夹取与 disocclusion。
struct D3D12TaaParameters
{
    float width=0,height=0;
    float staticBlend=0.1f,dynamicBlend=0.2f;
    float depthThreshold=0.05f,velocityThreshold=0.01f;
    float sharpen=0.0f,historyValid=0.0f;
};
struct D3D12TaaOutput
{
    Microsoft::WRL::ComPtr<ID3D12Resource> velocity,historyColor[2],historyDepth[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors,rtvs,dsvs;
    D3D12_GPU_DESCRIPTOR_HANDLE colorSrv{},velocitySrv{},depthSrv{},
        historyColorSrv[2]{},historyDepthSrv[2]{},historyColorUav[2]{},historyDepthUav[2]{};
    // 解析根签名的表头：SRV 表随 previous 选择，UAV 表随 current 选择。
    D3D12_GPU_DESCRIPTOR_HANDLE srvTable[2]{},uavTable[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE velocityRtv{},depthDsv{};
    UINT width=0,height=0,historyIndex=0;
    UINT64 frames=0;
};
class D3D12Taa
{
public:
    static constexpr UINT descriptorCount=14;
    HRESULT Initialize(ID3D12Device& device,D3D12MemoryAllocator& memory,
        const std::filesystem::path& shaders,const Localization& text);
    bool Ready() const noexcept {return device_&&velocityRoot_&&velocityLegacy_&&velocityCompressed_&&resolveRoot_&&resolve_; }
    // hdr 为 R16G16B16A16_FLOAT、depth 为 D32_FLOAT/R32_FLOAT，尺寸一致。
    HRESULT CreateOutput(ID3D12Resource& hdr,ID3D12Resource& depth,UINT width,UINT height,D3D12TaaOutput& output) const;
    // 记录速度 Pass；结束时深度为 DEPTH_READ、速度为 RENDER_TARGET，由 Resolve 继续。
    HRESULT RecordVelocity(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,D3D12TaaOutput& output,
        std::span<const D3D12MeshDraw> draws,const se::Mat4& clipFromWorld,ID3D12Resource& depth) const;
    // 记录解析；结束时本次写入的历史颜色为 PIXEL_SHADER_RESOURCE，历史深度为 NON_PIXEL_SHADER_RESOURCE。
    HRESULT Resolve(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,D3D12TaaOutput& output,
        ID3D12Resource& currentColor,ID3D12Resource& depth,const D3D12TaaParameters& parameters) const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> velocityRoot_,resolveRoot_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> velocityLegacy_,velocityCompressed_,resolve_;
    D3D12MemoryAllocator* memory_=nullptr;
};
