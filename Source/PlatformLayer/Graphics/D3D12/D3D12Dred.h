#pragma once
#include <filesystem>

struct ID3D12Device;

namespace se::d3d12
{
// 必须在任何 D3D12 设备创建之前调用；失败只表示 DRED 不可用，不影响渲染。
bool EnableDred(long* result=nullptr) noexcept;
// 设备移除后调用；即使没有 breadcrumb 也写出报告头，返回文件是否成功打开。
bool DumpDred(ID3D12Device* device,const std::filesystem::path& path) noexcept;
}
