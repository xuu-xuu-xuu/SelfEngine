#include "D3D12Dred.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fstream>

namespace se::d3d12
{
namespace
{
void WriteAllocationNode(std::ofstream& out,const char* kind,const D3D12_DRED_ALLOCATION_NODE1* node)
{
    for(;node;node=node->pNext)
        out<<kind<<": "<<(node->ObjectNameA?node->ObjectNameA:"(unnamed)")
           <<", type="<<static_cast<int>(node->AllocationType)<<'\n';
}
}

bool EnableDred(long* result) noexcept
{
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    HRESULT hr=DXGIGetDebugInterface1(0,IID_PPV_ARGS(settings.GetAddressOf()));
    if(FAILED(hr))
    {
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> base;
        hr=DXGIGetDebugInterface1(0,IID_PPV_ARGS(base.GetAddressOf()));
        if(result)*result=hr;
        if(FAILED(hr))return false;
        base->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        base->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        return true;
    }
    if(result)*result=hr;
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    return true;
}

bool DumpDred(ID3D12Device* device,const std::filesystem::path& path) noexcept
{
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out)return false;
    out<<"DRED report (device removed diagnostics)\n";
    if(!device)
    {
        out<<"device: unavailable\n";
        return true;
    }
    Microsoft::WRL::ComPtr<ID3D12Device5> device5;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(device5.GetAddressOf()))))
    {
        out<<"device5: unavailable\n";
        return true;
    }
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> data;
    if(FAILED(device5->QueryInterface(IID_PPV_ARGS(data.GetAddressOf()))))
    {
        out<<"dred: unavailable\n";
        return true;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    if(SUCCEEDED(data->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
    {
        for(const auto* node=breadcrumbs.pHeadAutoBreadcrumbNode;node;node=node->pNext)
        {
            out<<"command list: "<<(node->pCommandListDebugNameA?node->pCommandListDebugNameA:"(unnamed)")
               <<", queue: "<<(node->pCommandQueueDebugNameA?node->pCommandQueueDebugNameA:"(unnamed)")
               <<", breadcrumbs="<<node->BreadcrumbCount<<'\n';
            if(node->pLastBreadcrumbValue)
                out<<"  last completed breadcrumb: "<<*node->pLastBreadcrumbValue<<'\n';
            if(node->pCommandHistory)
                for(UINT index=0;index<node->BreadcrumbCount;++index)
                    out<<"  op["<<index<<"]="<<static_cast<int>(node->pCommandHistory[index])<<'\n';
        }
    }
    else out<<"breadcrumbs: unavailable\n";
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if(SUCCEEDED(data->GetPageFaultAllocationOutput1(&fault)))
    {
        out<<"page fault VA: 0x"<<std::hex<<fault.PageFaultVA<<std::dec<<'\n';
        WriteAllocationNode(out,"existing allocation",fault.pHeadExistingAllocationNode);
        WriteAllocationNode(out,"recent freed allocation",fault.pHeadRecentFreedAllocationNode);
    }
    else out<<"page fault: unavailable\n";
    return true;
}
}
