#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <memory>

class Localization;
class D3D12StateRegistry;
class D3D12DescriptorPool;
class D3D12MemoryAllocator;
class D3D12PipelineCache;

// 应用持有到退出；成员按声明的相反顺序释放：设备、适配器、工厂。
struct D3D12Context
{
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::shared_ptr<D3D12StateRegistry> states;
    std::shared_ptr<D3D12DescriptorPool> srvDescriptors,rtvDescriptors,dsvDescriptors;
    std::shared_ptr<D3D12MemoryAllocator> memory;
    std::shared_ptr<D3D12PipelineCache> pipelines;
};

// 启动时调用一次，在任何 D3D12 设备创建前启用调试层。
// 只接受空 context；所有步骤成功后才交付资源，失败不留下半初始化状态。
bool InitializeD3D12Context(D3D12Context& context, const Localization& localization);
