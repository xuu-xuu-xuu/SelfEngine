#pragma once
#include "D3D12Descriptors.h"
#include "D3D12Exposure.h"
#include "D3D12Taa.h"
#include <wrl/client.h>
#include <array>
struct D3D12Presentation;
struct D3D12FrameContext;
struct D3D12FrameContent;
class D3D12StateRecording;
class Localization;
struct D3D12PostTexture
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    std::shared_ptr<D3D12DescriptorRange> srv,rtv;
};
// 窗口 resize 先 drain，再替换整组；开关不会重建目标。
struct D3D12PostTargets
{
    std::array<D3D12PostTexture,2> bloom;
    D3D12PostTexture ldr;
    std::shared_ptr<D3D12DescriptorRange> outputRtvs;
    // G15：直方图/曝光/LUT 与后处理目标同寿命；resize 时整组重建。
    std::shared_ptr<D3D12Exposure> exposureKernel;
    D3D12ExposureOutput exposure;
    // ToneMap 表从持久 SRV 池复制，和 Bloom/LDR 走同一描述符路径。
    std::shared_ptr<D3D12DescriptorRange> exposureSrvs;
    bool exposureHistoryValid=false,exposureWasEnabled=false,lutDirty=true;
    // G15：`.selut` 资产只在 revision 变化时上传；切回 LGG 时标记 lutDirty 重新生成。
    bool lutAssetActive=false,lutAssetUploaded=false;
    std::uint64_t lutAssetRevision=0;
    // G14：只在首次启用 TAA 时创建；resize 时随整组目标重建。
    std::shared_ptr<D3D12Taa> taaKernel;
    D3D12TaaOutput taa;
    std::shared_ptr<D3D12DescriptorRange> taaSrvs;
    float lutLift[3]{0,0,0},lutGamma[3]{1,1,1},lutGain[3]{1,1,1};
    UINT width=0,height=0,bloomWidth=0,bloomHeight=0;
    UINT64 allocatedBytes=0;
};
bool PrepareD3D12PostProcess(D3D12Presentation& presentation,D3D12FrameContext& frame,const D3D12FrameContent& content,const Localization& text);
bool RecordD3D12PostProcess(D3D12Presentation& presentation,D3D12FrameContext& frame,const D3D12FrameContent& content,D3D12StateRecording& states,UINT backBufferIndex,const Localization& text);
