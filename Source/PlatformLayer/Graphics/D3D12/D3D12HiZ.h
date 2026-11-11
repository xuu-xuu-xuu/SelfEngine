#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

struct D3D12Context;
class Localization;
class D3D12StateRecording;
class D3D12MemoryAllocator;

// G06.03：GPU Hi-Z 资源。深度源只读一次，HZB 以 R32_FLOAT 全 mip 链保存；
// mip0 与深度同尺寸，后续每级取 2×2 最大值。所有权随 Output 持有，
// 输出被替换或销毁时由 shared_ptr/COM 释放对应分配。
struct D3D12HiZOutput
{
    Microsoft::WRL::ComPtr<ID3D12Resource> depth,texture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{},hzbSrv{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE,16> levels{}; // 每级 UAV（归约目标，u1）。
    UINT width=0,height=0,mipCount=0;
};
// 可选时间戳借用：0 起始、1 复制后、2 最后一级归约后；记录方不创建读回或等待。
struct D3D12HiZTimestamps {ID3D12QueryHeap* heap=nullptr;UINT capacity=0;};
class D3D12HiZ
{
public:
    static constexpr UINT maximumLevels=16,queryCount=3;
    HRESULT Initialize(D3D12Context& context,const std::filesystem::path& shader,const Localization& text);
    // 深度描述必须是单层、单 mip 的 R32_FLOAT 或 D32_FLOAT；失败不替换 output。
    HRESULT CreateOutput(ID3D12Resource& depth,UINT width,UINT height,D3D12HiZOutput& output) const;
    // 记录复制与逐级归约；结束时 HZB 处于 NON_PIXEL_SHADER_RESOURCE 供遮挡读取。
    HRESULT Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
        const D3D12HiZOutput& output,D3D12HiZTimestamps timestamps={},UINT firstQuery=0) const;
    static UINT Levels(UINT width,UINT height); // 0 表示尺寸无效或超过 maximumLevels。
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> copy_,reduce_;
    std::shared_ptr<D3D12MemoryAllocator> memory_;
};
