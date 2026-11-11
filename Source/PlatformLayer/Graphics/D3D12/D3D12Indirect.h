#pragma once
#include "D3D12Visibility.h"
#include "PlatformLayer/Graphics/MaterialLayout.h"

struct D3D12IndirectOutput
{
    Microsoft::WRL::ComPtr<ID3D12Resource> localOffsets,blockOffsets,instances,arguments;
    UINT inputCapacity=0,outputCapacity=0;
};
struct D3D12IndirectPrepared
{
private:
    friend class D3D12Indirect;
    D3D12UploadSlice bounds,instances,constants;
    UINT count=0,capacity=0;
    bool occlusion=false;
};
// G06.03：可选 Hi-Z 遮挡输入。hzb 必须是已构建好、处于 NON_PIXEL_SHADER_RESOURCE 的
// D3D12HiZOutput；heap/srv 在其生命周期内保持有效。
struct D3D12IndirectOcclusion
{
    ID3D12DescriptorHeap* heap=nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE srv{};
    se::Mat4 clipFromWorld;
    UINT width=0,height=0,mipCount=0;
    float depthBias=1e-6f;
};
// 调用期间借用，所有资源与kernel须保持到对应Fence完成。
struct D3D12IndirectCommand
{
    ID3D12CommandSignature* signature=nullptr;
    ID3D12Resource* arguments=nullptr;
};
class D3D12Indirect
{
public:
    static constexpr UINT argumentBytes=48,commandCountOffset=20;
    HRESULT Initialize(D3D12Context& context,const std::filesystem::path& shader,const Localization& text);
    HRESULT CreateOutput(UINT inputCapacity,UINT outputCapacity,D3D12IndirectOutput& output) const;
    HRESULT Prepare(D3D12UploadArena& arena,std::span<const se::VisibilityItem> items,const se::VisibilityFrustum& frustum,
        D3D12UploadSlice instanceData,UINT indexCount,UINT capacity,D3D12IndirectPrepared& output,
        const D3D12IndirectOcclusion* occlusion=nullptr) const;
    // 四个查询：初始状态前、局部前缀后、组前缀后、Gather后；输出仍为UAV，由消费方声明读用途。
    HRESULT Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,const D3D12IndirectPrepared& prepared,
        const D3D12IndirectOutput& output,D3D12VisibilityTimestamps timestamps={},UINT firstQuery=0,
        const D3D12IndirectOcclusion* occlusion=nullptr) const;
    D3D12IndirectCommand Command(const D3D12IndirectOutput& output) const {return {signature_.Get(),output.arguments.Get()};}
    ID3D12Device* Device() const {return device_.Get();}
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> local_,groups_,gather_;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> signature_;
    std::shared_ptr<D3D12MemoryAllocator> memory_;
};

// 受限Native实验接入；调用时借用输入，kernel/输出/查询保持至对应Presentation Fence完成。
class D3D12HiZ;
class D3D12DepthPrepass;
struct D3D12HiZOutput;
struct D3D12IndirectBatch
{
    const D3D12Indirect* kernel=nullptr;
    const D3D12IndirectOutput* output=nullptr;
    std::span<const se::VisibilityItem> bounds;
    se::VisibilityFrustum frustum;
    D3D12VisibilityTimestamps timestamps; // 使用0..4，最后一个查询覆盖消费屏障/绑定/间接绘制。
    // G06.11：多批次时每批次占用独立的 4 个 cull 查询；默认 0 保持实验路径。
    UINT timestampFirstQuery=0;
    ID3D12Resource* timestampReadback=nullptr; // 可选，与timestamps一起提供；至少40字节READBACK。
    // G06.09：可选，间接参数完成后复制到该 READBACK（至少 argumentBytes），供 CPU 异步读取
    // visible/candidates/overflow；不改变绘制参数和状态，复制后由 RHI 决定何时复用。
    // G06.10：多批次共用一块 READBACK，offset 指定本批次 48B 的位置。
    ID3D12Resource* countReadback=nullptr;
    UINT countReadbackOffset=0;
    // G06.05：可选 depth prepass + HZB 遮挡。三者必须同时提供；hiz 必须由当前 scene depth 创建。
    const D3D12HiZ* hizKernel=nullptr;
    const D3D12HiZOutput* hiz=nullptr;
    const D3D12DepthPrepass* prepass=nullptr;
    float depthBias=1e-6f;
    // 可选 HZB 时间戳（3 个，起始/复制后/最后一级后）；容量不足由 HiZ 记录前拒绝。
    ID3D12QueryHeap* hizQueries=nullptr;
    UINT hizQueryCapacity=0,hizFirstQuery=0;
};
