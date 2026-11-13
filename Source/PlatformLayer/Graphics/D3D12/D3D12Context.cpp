#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "D3D12StateTracker.h"
#include "D3D12Descriptors.h"
#include "D3D12Memory.h"
#include "D3D12PipelineCache.h"
#include "D3D12Dred.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Result.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"

#include <d3d12sdklayers.h>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
    // 当前基础渲染路径的最低门槛；不是整台显卡的最高能力。
    constexpr D3D_FEATURE_LEVEL minimumFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    constexpr D3D_SHADER_MODEL minimumShaderModel = D3D_SHADER_MODEL_5_1;

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
    bool CreateFactory(D3D12Context& context, const Localization& localization)
    {
        UINT factoryFlags = 0;
#ifndef NDEBUG
        wchar_t disableDebug[8]{};
        const bool debugDisabled = GetEnvironmentVariableW(L"SELFENGINE_DISABLE_D3D12_DEBUG", disableDebug, 8) > 0;
        if (!debugDisabled)
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
        else
        {
            Log(LogLevel::Info, localization.GetText("D3D12.DebugLayerSkipped"));
        }
#else
        Log(LogLevel::Info, localization.GetText("D3D12.DebugLayerSkipped"));
#endif

        return CheckGraphicsResult(
            CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(context.factory.GetAddressOf())),
            localization, "D3D12.CreateFactoryFailed");
    }

    bool LogAdapters(IDXGIFactory1& factory, const Localization& localization)
    {
        UINT adapterCount = 0;
        for (UINT index = 0; ; ++index)
        {
            // 每轮持有一个适配器引用，离开本轮作用域后自动 Release。
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory.EnumAdapters1(index, adapter.GetAddressOf());
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
            Log(LogLevel::Debug, message.str());
            ++adapterCount;
        }

        if (adapterCount == 0)
        {
            Log(LogLevel::Error, localization.GetText("D3D12.NoAdaptersFound"));
            return false;
        }
        Log(LogLevel::Debug, localization.GetText("D3D12.AdapterCount")
            + ": " + std::to_string(adapterCount));
        return true;
    }

    bool SelectAndCreateDevice(D3D12Context& context, const Localization& localization)
    {
        const auto featureValue = static_cast<UINT>(minimumFeatureLevel);
        const auto shaderValue = static_cast<UINT>(minimumShaderModel);
        Log(LogLevel::Info, localization.GetText("D3D12.MinimumRequirements")
            + ": Feature Level " + std::to_string(featureValue >> 12) + "_"
            + std::to_string((featureValue >> 8) & 0xF)
            + ", Shader Model " + std::to_string(shaderValue >> 4) + "_"
            + std::to_string(shaderValue & 0xF));
        Log(LogLevel::Debug, localization.GetText("D3D12.SelectionPolicy"));

        for (UINT index = 0; ; ++index)
        {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT result = context.factory->EnumAdapterByGpuPreference(index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(candidate.GetAddressOf()));
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (!CheckGraphicsResult(result, localization, "D3D12.EnumerateByPreferenceFailed"))
            {
                return false;
            }

            DXGI_ADAPTER_DESC1 description{};
            if (!CheckGraphicsResult(candidate->GetDesc1(&description), localization,
                "D3D12.GetAdapterDescriptionFailed"))
            {
                return false;
            }
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

            std::string name;
            if (!ConvertAdapterName(description.Description, name, localization)) return false;
            Log(LogLevel::Debug, localization.GetText("D3D12.Candidate") + ": " + name);

            ComPtr<ID3D12Device> candidateDevice;
            if (!CheckGraphicsResult(D3D12CreateDevice(candidate.Get(), minimumFeatureLevel,
                IID_PPV_ARGS(candidateDevice.GetAddressOf())), localization,
                "D3D12.CandidateDeviceFailed", LogLevel::Warning))
            {
                Log(LogLevel::Warning, localization.GetText("D3D12.Candidate") + ": " + name);
                continue; // 已记录候选型号与 HRESULT，再检查下一个硬件候选。
            }

            D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{}; // 当前只检查节点 0。
            if (!CheckGraphicsResult(candidateDevice->CheckFeatureSupport(
                D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture)),
                localization, "D3D12.QueryArchitectureFailed"))
            {
                Log(LogLevel::Error, localization.GetText("D3D12.Candidate") + ": " + name);
                return false;
            }
            if (architecture.UMA)
            {
                Log(LogLevel::Debug, localization.GetText("D3D12.SkipUmaAdapter"));
                continue;
            }

            // 请求 5_1 只验证最低要求，不能将返回值称为硬件最高 Shader Model。
            D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ minimumShaderModel };
            if (!CheckGraphicsResult(candidateDevice->CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)),
                localization, "D3D12.QueryShaderModelFailed"))
            {
                Log(LogLevel::Error, localization.GetText("D3D12.Candidate") + ": " + name);
                return false;
            }
            if (shaderModel.HighestShaderModel < minimumShaderModel)
            {
                Log(LogLevel::Warning, localization.GetText("D3D12.ShaderModelTooLow") + ": " + name);
                continue;
            }

            context.adapter = std::move(candidate);
            context.device = std::move(candidateDevice);
            Log(LogLevel::Info, localization.GetText("D3D12.SelectedAdapter") + ": " + name);
            Log(LogLevel::Info, localization.GetText("D3D12.MinimumCapabilitiesVerified"));
            Log(LogLevel::Info, localization.GetText("D3D12.DeviceCreated"));
            return true;
        }

        Log(LogLevel::Error, localization.GetText("D3D12.NoSuitableAdapter"));
        return false;
    }
}

bool InitializeD3D12Context(D3D12Context& context, const Localization& localization)
{
    if (context.factory || context.adapter || context.device || context.states || context.srvDescriptors || context.rtvDescriptors || context.dsvDescriptors || context.memory || context.pipelines)
    {
        Log(LogLevel::Error, localization.GetText("D3D12.ContextNotEmpty"));
        return false;
    }

    D3D12Context initialized;
    if (!CreateFactory(initialized, localization)) return false;
    // DRED 设置必须在设备创建前生效；调试层由 CreateFactory 先启用。
    long dredResult=0;
    const bool dred=se::d3d12::EnableDred(&dredResult);
    {std::ostringstream message;message<<localization.GetText(dred?"D3D12.DredEnabled":"D3D12.DredUnavailable")
        <<" (0x"<<std::hex<<static_cast<unsigned long>(dredResult)<<")";Log(dred?LogLevel::Info:LogLevel::Warning,message.str());}
    if (!LogAdapters(*initialized.factory.Get(), localization)
        || !SelectAndCreateDevice(initialized, localization))
    {
        return false;
    }
    initialized.states=D3D12StateRegistry::ForDevice(*initialized.device.Get());
    if(!initialized.states){Log(LogLevel::Error,localization.GetText("D3D12.StateTrackingFailed"));return false;}
    initialized.srvDescriptors=std::make_shared<D3D12DescriptorPool>(*initialized.device.Get(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    initialized.rtvDescriptors=std::make_shared<D3D12DescriptorPool>(*initialized.device.Get(),D3D12_DESCRIPTOR_HEAP_TYPE_RTV,8,1024);
    initialized.dsvDescriptors=std::make_shared<D3D12DescriptorPool>(*initialized.device.Get(),D3D12_DESCRIPTOR_HEAP_TYPE_DSV,8,1024);
    initialized.memory=std::make_shared<D3D12MemoryAllocator>(*initialized.device.Get(),initialized.states);
    initialized.pipelines=std::make_shared<D3D12PipelineCache>(*initialized.device.Get());
    {const auto cache=initialized.pipelines->Stats();Log(LogLevel::Debug,localization.GetText(cache.libraryLoaded?"D3D12.PsoLibraryLoaded":"D3D12.PsoLibraryNew")
        +": stores="+std::to_string(cache.libraryStores)+", hits="+std::to_string(cache.libraryHits));}
    context = std::move(initialized);
    return true;
}
