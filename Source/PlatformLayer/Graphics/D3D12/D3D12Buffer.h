#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <span>
#include <cstddef>
class Localization;
struct D3D12Context;
struct D3D12Presentation;
bool AllocateD3D12Buffer(Microsoft::WRL::ComPtr<ID3D12Resource>& output,const D3D12Context& context,
    UINT64 bytes,const Localization& text);
bool UploadD3D12Buffer(Microsoft::WRL::ComPtr<ID3D12Resource>& output,const D3D12Context& context,
    D3D12Presentation& presentation,std::span<const std::byte> bytes,D3D12_RESOURCE_STATES usage,const Localization& text);
