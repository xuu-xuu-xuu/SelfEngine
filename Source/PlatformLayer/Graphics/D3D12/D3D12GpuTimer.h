#pragma once
struct D3D12Presentation;
class Localization;
bool SetD3D12GpuTiming(D3D12Presentation& presentation,bool enabled,const Localization& text);
bool ReadD3D12GpuTimings(D3D12Presentation& presentation,const Localization& text);
