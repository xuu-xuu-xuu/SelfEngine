#pragma once
#include "D3D12Gui.h"
#include <array>

struct D3D12FrameContent;
// 每个帧槽单独拥有掩码和描述符；仅在该槽 Fence 完成后替换或复用。
struct D3D12SceneOverlayFrame
{
    D3D12GuiTarget mask; // R：模型完整投影；G：通过场景深度测试的可见像素。
    D3D12FrameDescriptors descriptors;
};
class D3D12SceneOverlay
{
public:
    bool Record(D3D12Presentation&,D3D12FrameContext&,const D3D12FrameContent&,
        D3D12StateRecording&,const Localization&);
private:
    bool Initialize(ID3D12Device&,const std::filesystem::path&,const Localization&);
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    enum PipelineSlot : UINT
    {
        MaskVisible,MaskVisiblePacked,LinesVisible,LinesHidden,Outline,MaskFull,MaskFullPacked,GroundGrid,PipelineCount
    };
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,PipelineCount> pipelines_;
};
