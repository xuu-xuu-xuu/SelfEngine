#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>

class Localization;
struct D3D12Mesh;
class D3D12DepthPrepass
{
public:
    // root 必须与深度 PSO 使用同一布局；实例 SRV 的根参数由调用方指定：
    // 非 Forward 实例化为 2（t7），G06.10 的 Forward 实例化为 3（b1 之后）。
    HRESULT Initialize(ID3D12Device& device,const std::filesystem::path& shader,ID3D12RootSignature& root,
        const Localization& text,UINT instanceParameter=2);
    bool Ready() const noexcept {return pipeline_!=nullptr;}
    UINT InstanceParameter() const noexcept {return instanceParameter_;}
    void Record(ID3D12GraphicsCommandList& commands,const D3D12Mesh& mesh,D3D12_GPU_VIRTUAL_ADDRESS instanceSrv,
        UINT width,UINT height,UINT instanceCount) const;
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    UINT instanceParameter_=2;
};
