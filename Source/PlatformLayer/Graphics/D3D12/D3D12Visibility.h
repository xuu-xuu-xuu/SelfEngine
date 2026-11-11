#pragma once
#include "PlatformLayer/Graphics/VisibilityData.h"
#include "D3D12UploadArena.h"
#include <filesystem>

struct D3D12Context;
class D3D12MemoryAllocator;
class D3D12StateRecording;
class Localization;
struct D3D12VisibilityOutput
{
    Microsoft::WRL::ComPtr<ID3D12Resource> ids,counters;
    UINT capacity=0;
};
// 借用上传页，调用者必须保持到对应提交完成；不得跨页Reset使用。
struct D3D12VisibilityPrepared
{
private:
    friend class D3D12Visibility;
    D3D12UploadSlice items,constants;
    UINT count=0,capacity=0;
};
// capacity 必须是创建 heap 时的真实Count；D3D12查询堆不提供描述查询接口。
struct D3D12VisibilityTimestamps {ID3D12QueryHeap* heap=nullptr;UINT capacity=0;};
class D3D12Visibility
{
public:
    HRESULT Initialize(D3D12Context& context,const std::filesystem::path& shader,const Localization& text);
    HRESULT CreateOutput(UINT capacity,D3D12VisibilityOutput& output) const;
    HRESULT Prepare(D3D12UploadArena& arena,std::span<const se::VisibilityItem> items,const se::VisibilityFrustum& frustum,
        UINT capacity,D3D12VisibilityPrepared& output) const;
    // 可选时间戳：Reset前、Reset后、UAV依赖及Cull后；不创建读回或等待。
    HRESULT Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,const D3D12VisibilityPrepared& prepared,
        const D3D12VisibilityOutput& output,D3D12VisibilityTimestamps timestamps={},UINT firstQuery=0) const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> reset_,cull_;
    std::shared_ptr<D3D12MemoryAllocator> memory_;
};
