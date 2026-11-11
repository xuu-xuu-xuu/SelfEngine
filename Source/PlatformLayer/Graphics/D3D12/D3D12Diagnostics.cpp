#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <sstream>
#include <string>5
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace
{
    // 图形诊断使用已加载的语言表；原有 T02 HRESULT 练习暂不迁移。
    bool CheckGraphicsResult(HRESULT result, const Localization& localization,
        std::string_view errorKey)
    {
        if (SUCCEEDED(result)) return true;

        std::ostringstream message;
        message << localization.GetText(errorKey) << " (HRESULT=0x"
            << std::hex << static_cast<unsigned long>(result) << ')';
        Log(LogLevel::Error, message.str());
        return false;
    }

    bool ConvertAdapterName(const wchar_t* name, std::string& utf8,
        const Localization& localization)
    {
        // DXGI 提供 UTF-16，现有日志输出 UTF-8；先查询包含末尾零字符的大小。
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            name, -1, nullptr, 0, nullptr, nullptr);
        if (size == 0)
        {
            CheckGraphicsResult(HRESULT_FROM_WIN32(GetLastError()), localization,
                "D3D12.AdapterNameConversionFailed");
            return false;
        }

        utf8.resize(size);
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            name, -1, utf8.data(), size, nullptr, nullptr) == 0)
        {
            CheckGraphicsResult(HRESULT_FROM_WIN32(GetLastError()), localization,
                "D3D12.AdapterNameConversionFailed");
            return false;
        }
        utf8.pop_back(); // std::string 的内容不包含 Windows 写入的末尾零字符。
        return true;
    }
}

bool RunD3D12Diagnostics(const Localization& localization)
{
    UINT factoryFlags = 0;
#ifndef NDEBUG
    {
        ComPtr<ID3D12Debug> debugController;
        if (!CheckGraphicsResult(
            D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf())),
            localization, "D3D12.DebugLayerUnavailable"))
        {
            return false;
        }
        // 必须先启用，再创建设备；释放此接口不会撤销启用设置。
        debugController->EnableDebugLayer();
        factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
        Log(LogLevel::Info, localization.GetText("D3D12.DebugLayerEnabled"));
    }
#else
    Log(LogLevel::Info, localization.GetText("D3D12.DebugLayerSkipped"));
#endif

    ComPtr<IDXGIFactory1> factory;
    if (!CheckGraphicsResult(
        CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory.GetAddressOf())),
        localization, "D3D12.CreateFactoryFailed"))
    {
        return false;
    }

    UINT adapterCount = 0;
    for (UINT index = 0; ; ++index)
    {
        // 每轮持有一个适配器引用，离开本轮作用域后自动 Release。
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (result == DXGI_ERROR_NOT_FOUND) break; // 正常枚举结束，不是初始化失败。
        if (!CheckGraphicsResult(result, localization, "D3D12.EnumerateAdaptersFailed"))
        {
            return false;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (!CheckGraphicsResult(adapter->GetDesc1(&description), localization,
            "D3D12.GetAdapterDescriptionFailed"))
        {
            return false;
        }

        std::string name;
        if (!ConvertAdapterName(description.Description, name, localization))
        {
            return false;
        }
        const bool isSoftware = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        std::ostringstream message;
        message << localization.GetText("D3D12.Adapter") << " [" << index << "]: "
            << name << " (" << localization.GetText(isSoftware
                ? "D3D12.AdapterSoftware" : "D3D12.AdapterHardware") << ')';
        Log(LogLevel::Info, message.str());
        ++adapterCount;
    }

    if (adapterCount == 0)
    {
        Log(LogLevel::Error, localization.GetText("D3D12.NoAdaptersFound"));
        return false;
    }
    Log(LogLevel::Info, localization.GetText("D3D12.AdapterCount")
        + ": " + std::to_string(adapterCount));
    return true;
}
