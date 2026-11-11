#pragma once
#include <d3d12.h>
struct D3D12Presentation;
struct D3D12FrameContext;
struct D3D12FrameContent;
class Localization;
bool PrepareD3D12Forward(D3D12Presentation& presentation,D3D12FrameContext& frame,
    const D3D12FrameContent& content,const Localization& text);
void RecordD3D12ForwardOutput(D3D12Presentation& presentation,D3D12FrameContext& frame,
    const D3D12FrameContent& content,UINT width,UINT height,D3D12_CPU_DESCRIPTOR_HANDLE rtv);
void RecordD3D12ForwardSky(D3D12Presentation& presentation,D3D12FrameContext& frame,const D3D12FrameContent& content,
    UINT width,UINT height);
