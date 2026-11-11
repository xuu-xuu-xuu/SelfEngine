#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Triangle.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Result.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Profiler.h"
#include "D3D12GpuTimer.h"
#include "D3D12StateTracker.h"
#include "D3D12Memory.h"
#include "D3D12PipelineCache.h"
#include "D3D12Forward.h"
#include "D3D12Shadow.h"
#include "D3D12PostProcess.h"
#include "D3D12Gui.h"
#include "D3D12Indirect.h"
#include "D3D12HiZ.h"
#include "D3D12DepthPrepass.h"
#include "D3D12Dred.h"

#include <algorithm>
#include <exception>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
bool CreateDepth(D3D12Presentation& presentation,ID3D12Device& device,UINT width,UINT height,const Localization& text)
{
    D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width=width;description.Height=height;description.DepthOrArraySize=description.MipLevels=1;
    description.Format=D3D12Presentation::depthFormat;description.SampleDesc={1,0};description.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};clear.Format=D3D12Presentation::depthFormat;clear.DepthStencil.Depth=1;
    ComPtr<ID3D12Resource> depth;
    if(!presentation.memory)return false;
    const auto created=presentation.memory->Create(description,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,depth,"Depth");
    if(!CheckGraphicsResult(created,text,created==D3D12MemoryBudgetExceeded?"D3D12.GpuMemoryBudgetExceeded":"D3D12.CreateDepthFailed"))return false;
    device.CreateDepthStencilView(depth.Get(),nullptr,presentation.dsv->Cpu());
    presentation.depth=std::move(depth);
    return true;
}

bool CreateConstants(D3D12FrameContext& frame,ID3D12Device& device,std::shared_ptr<D3D12StateRegistry> states,const Localization& text)
{
    frame.uploads.Initialize(device,std::move(states));D3D12UploadSlice warm;
    const auto result=frame.uploads.Allocate(256,256,warm);frame.uploads.Reset();
    return CheckGraphicsResult(result,text,"D3D12.CreateConstantsFailed");
}

HRESULT DeviceRemovalReason(D3D12Presentation& presentation)
{
    ComPtr<ID3D12Device> device;
    if (!presentation.queue || FAILED(presentation.queue->GetDevice(
        IID_PPV_ARGS(device.GetAddressOf())))) return S_OK;
    return device->GetDeviceRemovedReason();
}

bool DeviceWasRemoved(D3D12Presentation& presentation)
{
    return (presentation.fence && presentation.fence->GetCompletedValue() == UINT64_MAX)
        || FAILED(DeviceRemovalReason(presentation));
}

void ReportDeviceRemoval(D3D12Presentation& presentation, const Localization& localization)
{
    HRESULT reason = DeviceRemovalReason(presentation);
    if (SUCCEEDED(reason)) reason = DXGI_ERROR_DEVICE_REMOVED;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (presentation.queue && SUCCEEDED(presentation.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))
        try
        {
            std::error_code error;
            const auto directory = std::filesystem::current_path(error) / "out" / "logs";
            if (!error) std::filesystem::create_directories(directory, error);
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm local{};
            localtime_s(&local, &now);
            std::ostringstream name;
            name << "gpu-crash-" << std::put_time(&local, "%Y%m%d-%H%M%S") << ".txt";
            const auto path = directory / name.str();
            if (!error && se::d3d12::DumpDred(device.Get(), path))
                Log(LogLevel::Warning, localization.GetText("D3D12.DredDumpWritten") + ": " + path.string());
            else Log(LogLevel::Warning, localization.GetText("D3D12.DredDumpFailed"));
        }
        catch (...) { Log(LogLevel::Warning, localization.GetText("D3D12.DredDumpFailed")); }
    CheckGraphicsResult(reason, localization, "D3D12.DeviceRemoved");
}

HRESULT SignalSubmittedWork(D3D12Presentation& presentation, UINT64& value)
{
    // UINT64_MAX 是设备移除哨兵，不分配为正常完成值。
    if (presentation.nextFenceValue == UINT64_MAX) return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    value = presentation.nextFenceValue;
    const HRESULT result = presentation.queue->Signal(presentation.fence.Get(), value);
    if (SUCCEEDED(result))
    {
        ++presentation.nextFenceValue;
        ++presentation.signalCount;
        presentation.untrackedSubmission = false;
    }
    return result;
}

HRESULT WaitForFence(D3D12Presentation& presentation, UINT64 value, D3D12WaitReason reason)
{
    UINT64 completed = presentation.fence->GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (completed >= value) return S_OK;
    se::CpuScope waitScope(presentation.cpuProfiler,"CPU.GpuFenceWait");
    ++presentation.waitCounts[static_cast<size_t>(reason)];
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (completed < value)
    {
        const HRESULT armed = presentation.fence->SetEventOnCompletion(value, presentation.fenceEvent.Get());
        if (FAILED(armed)) return armed;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        const DWORD result = WaitForSingleObject(presentation.fenceEvent.Get(), static_cast<DWORD>(deadline - now));
        if (result == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
        if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) return E_FAIL;
        completed = presentation.fence->GetCompletedValue();
        if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        // 事件可能来自此前较小的完成值；重新查询，不能把事件本身当成完成证明。
        if (result == WAIT_TIMEOUT && completed < value) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return S_OK;
}

HRESULT FlushQueue(D3D12Presentation& presentation, D3D12WaitReason reason)
{
    ++presentation.flushCounts[static_cast<size_t>(reason)];
    UINT64 value = 0;
    // 值空间耗尽时，只要全部工作已有标记，仍可等待最后标记来安全关闭。
    HRESULT result = S_OK;
    if (presentation.nextFenceValue == UINT64_MAX && !presentation.untrackedSubmission)
        value = UINT64_MAX - 1;
    else result = SignalSubmittedWork(presentation, value);
    if (SUCCEEDED(result)) result = WaitForFence(presentation, value, reason);
    if (SUCCEEDED(result) || DeviceWasRemoved(presentation))
    {
        presentation.gpuWorkPending = false;
        presentation.untrackedSubmission = false;
    }
    return result;
}

bool ValidCapture(D3D12Presentation& presentation, UINT index, const D3D12FrameCapture& capture,bool hdr=false,ID3D12Resource* resource=nullptr)
{
    if (!capture.readback) return false;
    ComPtr<ID3D12Device> device, captureDevice;
    if (FAILED(presentation.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))
        || FAILED(capture.readback->GetDevice(IID_PPV_ARGS(captureDevice.GetAddressOf())))
        || device.Get() != captureDevice.Get()) return false;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(capture.readback->GetHeapProperties(&heap, &flags)) || heap.Type != D3D12_HEAP_TYPE_READBACK)
        return false;
    const auto source = resource?resource->GetDesc():hdr?presentation.hdr->GetDesc():presentation.backBuffers[index]->GetDesc();
    const auto destination = capture.readback->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT expected{};
    UINT64 required = 0;
    device->GetCopyableFootprints(&source, 0, 1, 0, &expected, nullptr, nullptr, &required);
    const auto& actual = capture.footprint.Footprint;
    return destination.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && destination.Width >= required
        && capture.footprint.Offset == expected.Offset && actual.Format == expected.Footprint.Format
        && actual.Width == expected.Footprint.Width && actual.Height == expected.Footprint.Height
        && actual.Depth == expected.Footprint.Depth && actual.RowPitch == expected.Footprint.RowPitch;
}
}

bool InitializeD3D12Presentation(D3D12Presentation& presentation,
    const D3D12Context& context, HWND hwnd, const Localization& localization)
{
    const bool hasBackBuffers = std::any_of(presentation.backBuffers.begin(),
        presentation.backBuffers.end(), [](const auto& buffer) { return buffer.Get() != nullptr; });
    const bool hasAllocators = std::any_of(presentation.frames.begin(), presentation.frames.end(),
        [](const auto& frame) { return frame.allocator || frame.uploads.Stats().reservedBytes; });
    if (presentation.queue || presentation.swapChain || presentation.rtvs || hasBackBuffers
        || hasAllocators || presentation.commandList || presentation.fence || presentation.depth || presentation.dsv || presentation.memory
        || presentation.fenceEvent.IsValid())
    {
        Log(LogLevel::Error, localization.GetText("D3D12.PresentationNotEmpty"));
        return false;
    }
    if (!context.factory || !context.device || !context.states || !context.memory || !context.rtvDescriptors || !context.dsvDescriptors || hwnd == nullptr)
    {
        Log(LogLevel::Error, localization.GetText("D3D12.PresentationInputsInvalid"));
        return false;
    }

    RECT client{};
    if (!GetClientRect(hwnd, &client))
    {
        CheckGraphicsResult(HRESULT_FROM_WIN32(GetLastError()), localization,
            "D3D12.GetClientRectFailed");
        return false;
    }
    if (client.right <= client.left || client.bottom <= client.top)
    {
        Log(LogLevel::Error, localization.GetText("D3D12.InvalidSwapChainSize"));
        return false;
    }

    D3D12Presentation initialized;
    initialized.states=context.states;
    initialized.memory=context.memory;
    initialized.srvPool=context.srvDescriptors;initialized.rtvPool=context.rtvDescriptors;
    initialized.dsvPool=context.dsvDescriptors;
    struct InitialLifetime
    {
        D3D12Presentation& value;bool published=false;
        ~InitialLifetime(){if(!published)ReleaseD3D12Presentation(value);}
    } lifetime{initialized};
    initialized.width = static_cast<UINT>(client.right - client.left);
    initialized.height = static_cast<UINT>(client.bottom - client.top);

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!CheckGraphicsResult(context.device->CreateCommandQueue(&queueDescription,
        IID_PPV_ARGS(initialized.queue.GetAddressOf())), localization,
        "D3D12.CreateCommandQueueFailed"))
    {
        return false;
    }

    if(!CheckGraphicsResult(initialized.states->BindQueue(*initialized.queue.Get()),localization,"D3D12.StateTrackingFailed"))return false;
    initialized.queue->SetName(L"SelfEngine.Direct");
    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
    swapChainDescription.Width = initialized.width;
    swapChainDescription.Height = initialized.height;
    swapChainDescription.Format = D3D12Presentation::bufferFormat;
    swapChainDescription.SampleDesc = { 1, 0 }; // 交换链不使用 MSAA。
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = D3D12Presentation::bufferCount;
    swapChainDescription.Scaling = DXGI_SCALING_STRETCH;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain;
    // D3D12 在这里传直接队列；两个 nullptr 分别表示窗口模式、不限制输出显示器。
    if (!CheckGraphicsResult(context.factory->CreateSwapChainForHwnd(
        initialized.queue.Get(), hwnd, &swapChainDescription, nullptr, nullptr,
        swapChain.GetAddressOf()), localization, "D3D12.CreateSwapChainFailed"))
    {
        return false;
    }
    if (!CheckGraphicsResult(swapChain.As(&initialized.swapChain), localization,
        "D3D12.QuerySwapChainFailed"))
    {
        return false;
    }
    // 当前只支持窗口模式，避免 DXGI 自动切到尚未实现的独占全屏流程。
    if (!CheckGraphicsResult(context.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER),
        localization, "D3D12.DisableAltEnterFailed"))
    {
        return false;
    }

    if(!CheckGraphicsResult(context.rtvDescriptors->Allocate(D3D12Presentation::bufferCount,initialized.rtvs),
        localization,"D3D12.DescriptorCapacityExceeded"))return false;
    for (UINT index = 0; index < D3D12Presentation::bufferCount; ++index)
    {
        if (!CheckGraphicsResult(initialized.swapChain->GetBuffer(index,
            IID_PPV_ARGS(initialized.backBuffers[index].GetAddressOf())), localization,
            "D3D12.GetBackBufferFailed"))
        {
            Log(LogLevel::Error, localization.GetText("D3D12.BackBufferIndex")
                + ": " + std::to_string(index));
            return false;
        }
        // RTV 是描述符，不替代对纹理资源的 ComPtr 引用；此 API 返回 void。
        if(!CheckGraphicsResult(initialized.states->Register(*initialized.backBuffers[index].Get(),D3D12_RESOURCE_STATE_PRESENT,
            D3D12TrackedHeap::Default,"Backbuffer["+std::to_string(index)+"]"),localization,"D3D12.StateTrackingFailed"))return false;
        context.device->CreateRenderTargetView(initialized.backBuffers[index].Get(), nullptr, initialized.rtvs->Cpu(index));
    }

    if(!CheckGraphicsResult(context.dsvDescriptors->Allocate(1,initialized.dsv),localization,"D3D12.DescriptorCapacityExceeded")
        ||!CreateDepth(initialized,*context.device.Get(),initialized.width,initialized.height,localization))return false;
    for (auto& frame : initialized.frames)
    {
        if (!CheckGraphicsResult(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(frame.allocator.GetAddressOf())), localization,
            "D3D12.CreateCommandAllocatorFailed")) return false;
        if(!CreateConstants(frame,*context.device.Get(),initialized.states,localization))return false;
        if(!frame.outputDescriptors.Configure(1,1))return false;
    }
    if (!CheckGraphicsResult(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        initialized.frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(initialized.commandList.GetAddressOf())),
        localization, "D3D12.CreateCommandListFailed")) return false;
    // 新建列表处于录制状态，先关闭，以后每帧统一 Reset 后再录制。
    if (!CheckGraphicsResult(initialized.commandList->Close(), localization,
        "D3D12.CloseCommandListFailed")) return false;
    if (!CheckGraphicsResult(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(initialized.fence.GetAddressOf())), localization,
        "D3D12.CreateFenceFailed")) return false;
    initialized.fenceEvent.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!initialized.fenceEvent.IsValid())
    {
        CheckGraphicsResult(HRESULT_FROM_WIN32(GetLastError()), localization, "D3D12.CreateFenceEventFailed");
        return false;
    }

    presentation = std::move(initialized);
    lifetime.published=true;
    Log(LogLevel::Info, localization.GetText("D3D12.CommandQueueCreated"));
    Log(LogLevel::Info, localization.GetText("D3D12.SwapChainCreated") + ": "
        + std::to_string(presentation.width) + " x " + std::to_string(presentation.height));
    Log(LogLevel::Info, localization.GetText("D3D12.RenderTargetsReady")
        + ": " + std::to_string(D3D12Presentation::bufferCount));
    Log(LogLevel::Info, localization.GetText("D3D12.FrameContextsReady")
        + ": " + std::to_string(D3D12Presentation::frameCount));
    return true;
}

bool RenderD3D12Frame(D3D12Presentation& presentation, const Localization& localization,
    const D3D12FrameContent& content, const D3D12FrameCapture* capture)
{
    const auto* triangle=content.triangle;
    const auto& clipFromWorld=content.viewProjection;
    const bool forward=content.forward!=nullptr;
    const bool shadow=content.shadow!=nullptr;
    const bool post=content.post!=nullptr;
    const auto* gui=content.gui;
    const std::span<const D3D12IndirectBatch> indirects(content.indirects,content.indirectCount);
    const bool indirect=!indirects.empty();
    const bool telemetry=content.occlusionTelemetry!=nullptr;
    const UINT telemetryQueries=telemetry?static_cast<UINT>(6+4*indirects.size()):0;
    if(indirect)
    {
        ComPtr<ID3D12Device> device;
        // G06.10：Forward 实例化与遮挡可以同帧；HDR 目标在下方统一的 rtv 选择中处理。
        // GameUI 仍可作为场景后的覆盖层，但编辑器 GUI/合成/三角形路径不与间接批次共存。
        if(!presentation.queue||FAILED(presentation.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))
            ||content.gui||triangle||!content.meshPipeline||!content.meshPipeline->prepared
            ||content.meshes.empty()||content.meshes.size()!=indirects.size()||indirects.size()>D3D12MeshMaxDraws
            ||!se::rhi::IsInstanced(content.meshPipeline->prepared->program->source.kind))
        {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
        const bool hizExpected=bool(indirects.front().hiz);
        for(size_t index=0;index<indirects.size();++index)
        {
            const auto& batch=indirects[index];
            if(!batch.kernel||!batch.output||batch.kernel->Device()!=device.Get()
                ||batch.bounds.size()!=content.meshes[index].instances.size()
                ||batch.bounds.size()>se::MaximumVisibilityItems
                ||batch.output->inputCapacity<batch.bounds.size()||batch.output->outputCapacity<batch.bounds.size()
                ||!se::ValidVisibilityFrustum(batch.frustum)
                ||bool(batch.timestamps.heap)!=bool(batch.timestampReadback)
                ||bool(batch.hiz)!=hizExpected||bool(batch.hizKernel)!=hizExpected||bool(batch.prepass)!=hizExpected)
            {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
            // 单批次仍可记录 5/8 个时间戳；G06.11 telemetry 例外，按批次独立 cull 查询。
            if(batch.timestamps.heap)
            {
                ComPtr<ID3D12Device> queryDevice,readbackDevice;D3D12_HEAP_PROPERTIES heap{};
                if((!telemetry&&indirects.size()!=1)||batch.timestamps.capacity<5
                    ||FAILED(batch.timestamps.heap->GetDevice(IID_PPV_ARGS(queryDevice.GetAddressOf())))||queryDevice.Get()!=device.Get()
                    ||FAILED(batch.timestampReadback->GetDevice(IID_PPV_ARGS(readbackDevice.GetAddressOf())))||readbackDevice.Get()!=device.Get()
                    ||batch.timestampReadback->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||batch.timestampReadback->GetDesc().Width<40
                    ||FAILED(batch.timestampReadback->GetHeapProperties(&heap,nullptr))||heap.Type!=D3D12_HEAP_TYPE_READBACK)
                {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
            }
            // G06.09/G06.10：可选参数回读必须是本设备的 READBACK 缓冲；多批次共享并由 offset 分区。
            if(batch.countReadback)
            {
                ComPtr<ID3D12Device> readbackDevice;D3D12_HEAP_PROPERTIES heap{};
                const UINT64 required=UINT64{batch.countReadbackOffset}+D3D12Indirect::argumentBytes;
                if(FAILED(batch.countReadback->GetDevice(IID_PPV_ARGS(readbackDevice.GetAddressOf())))
                    ||readbackDevice.Get()!=device.Get()
                    ||batch.countReadback->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER
                    ||batch.countReadback->GetDesc().Width<required
                    ||batch.countReadbackOffset%4
                    ||FAILED(batch.countReadback->GetHeapProperties(&heap,nullptr))||heap.Type!=D3D12_HEAP_TYPE_READBACK)
                {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
            }
            if(hizExpected)
            {
                if(!batch.prepass->Ready()
                    ||batch.hiz->depth.Get()!=presentation.depth.Get()||batch.hiz->width!=presentation.width
                    ||batch.hiz->height!=presentation.height||!batch.hiz->mipCount
                    ||(batch.timestamps.heap&&(batch.hizQueries!=batch.timestamps.heap
                        ||batch.timestamps.capacity<8||batch.hizQueryCapacity<8||batch.hizFirstQuery>5))
                    ||!std::isfinite(batch.depthBias)||batch.depthBias<0||batch.depthBias>0.1f)
                {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
                // 主 Pass 只着色不写深度；prepass 已写入同一深度，必须使用 LESS_EQUAL + 写掩码 0。
                if(content.meshPipeline->prepared->depth!=se::rhi::DepthMode::ReadOnlyLessEqual)
                {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
            }
        }
        if(telemetry)
        {
            // G06.11：0/1 prepass、2..4 HZB、5 main end，批次 i 的 cull 为 6+4i..6+4i+3。
            const auto& first=indirects.front();
            if(!content.occlusionTelemetry->queries||!hizExpected||!first.timestamps.heap
                ||first.timestamps.heap!=content.occlusionTelemetry->queries||!first.timestampReadback
                ||first.timestampReadback->GetDesc().Width<UINT64{telemetryQueries}*sizeof(UINT64)
                ||first.hizQueries!=content.occlusionTelemetry->queries
                ||first.hizQueryCapacity<telemetryQueries||first.hizFirstQuery!=2u)
            {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
            for(size_t batchIndex=0;batchIndex<indirects.size();++batchIndex)
            {
                const auto& batch=indirects[batchIndex];
                if(batch.timestamps.heap!=content.occlusionTelemetry->queries
                    ||batch.timestamps.capacity<telemetryQueries
                    ||batch.timestampFirstQuery!=static_cast<UINT>(6+4*batchIndex)
                    ||batch.hizQueries!=content.occlusionTelemetry->queries
                    ||batch.hizQueryCapacity<telemetryQueries)
                {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
            }
        }
    }
    if(content.sceneHdr&&(!content.sceneLdr||!content.sceneHdr->color.resource||!content.sceneHdr->rtv||!content.sceneLdr->rtv
        ||content.sceneHdr->color.width!=content.sceneLdr->color.width||content.sceneHdr->color.height!=content.sceneLdr->color.height
        ||!content.sceneHdr->depth||!content.sceneHdr->dsv))return false;
    if(content.sceneHdr&&!forward)return false;
    const UINT sceneWidth=content.sceneHdr?content.sceneHdr->color.width:gui?gui->scene->color.width:presentation.width;
    const UINT sceneHeight=content.sceneHdr?content.sceneHdr->color.height:gui?gui->scene->color.height:presentation.height;
    auto* sceneDepth=content.sceneHdr?content.sceneHdr->depth.Get():gui?gui->scene->depth.Get():presentation.depth.Get();
    const auto sceneDsv=content.sceneHdr?content.sceneHdr->dsv:gui?gui->scene->dsv:presentation.dsv;
    if(post&&!forward)return false;
    if(forward&&(!content.outputPipeline||!content.outputPipeline->prepared
        ||!se::rhi::IsOutput(content.outputPipeline->prepared->program->source.kind)))return false;
    if(forward&&(!content.skyPipeline||!content.skyPipeline->prepared||!se::rhi::IsSky(content.skyPipeline->prepared->program->source.kind)))return false;
    if(content.meshPipeline&&(!content.meshPipeline->prepared||se::rhi::IsForward(content.meshPipeline->prepared->program->source.kind)!=forward
        ||se::rhi::IsShadowed(content.meshPipeline->prepared->program->source.kind)!=shadow
        ||(content.meshPipeline->prepared->program->source.kind==se::rhi::PipelineKind::EnvironmentMesh)!=(content.environment!=nullptr)))return false;
    if(content.environment&&(!shadow||content.skyPipeline->prepared->program->source.kind!=se::rhi::PipelineKind::EnvironmentSky
        ||std::any_of(content.environmentTextures.begin(),content.environmentTextures.end(),[](const auto* texture){return !texture||!texture->resource||!texture->views;})))return false;
    if(shadow&&(!forward||!content.shadowPipeline||!content.shadowPipeline->prepared
        ||content.shadowPipeline->prepared->program->source.kind!=se::rhi::PipelineKind::ShadowDepth
        ||content.outputPipeline->prepared->program->source.kind!=se::rhi::PipelineKind::ShadowOutput))return false;
    if(content.shadowCapture&&!shadow)return false;
    if(content.hdrCapture&&!forward)return false;
    if (!presentation.queue || !presentation.states || !presentation.swapChain || !presentation.rtvs
        || !presentation.commandList || !presentation.fence || !presentation.fenceEvent.IsValid()
        || presentation.failed || presentation.untrackedSubmission || presentation.uploadStates || !presentation.width || !presentation.height
        || presentation.frameCursor >= D3D12Presentation::frameCount
        || !presentation.frames[presentation.frameCursor].allocator
        || (triangle && (!triangle->pipeline || !triangle->rootSignature || !triangle->vertices || !triangle->prepared
            ||triangle->prepared->colorFormat!=se::rhi::ColorFormat::Rgba8Unorm||triangle->prepared->depth!=se::rhi::DepthMode::Disabled)))
    {
        Log(LogLevel::Error, localization.GetText("D3D12.ClearInputsInvalid"));
        return false;
    }
    if((triangle&&content.meshPipeline)||(!content.meshes.empty()&&!content.meshPipeline))
    {Log(LogLevel::Error,localization.GetText("D3D12.MeshDrawsInvalid"));return false;}
    if(content.meshPipeline)
    {
        se::CpuScope validation(presentation.cpuProfiler,"CPU.MeshValidation");
        if(!presentation.depth||!presentation.dsv
            ||!ValidateD3D12MeshDraws(*content.meshPipeline,content.meshes,clipFromWorld,localization,indirect))return false;
        // 多批次允许每个绘制使用自己的 prepared Pipeline；HZB 开启时逐一确认只读深度模式。
        if(indirect&&indirects.front().hiz)
            for(const auto& draw:content.meshes)
            {
                const auto& active=draw.pipeline?*draw.pipeline:*content.meshPipeline;
                if(active.prepared->depth!=se::rhi::DepthMode::ReadOnlyLessEqual)
                {Log(LogLevel::Error,localization.GetText("D3D12.IndirectInputsInvalid"));return false;}
            }
    }
    if (triangle && !se::IsFinite(clipFromWorld))
    {
        Log(LogLevel::Error, localization.GetText("Camera.InvalidProjection"));
        return false;
    }
    if (presentation.nextFenceValue >= UINT64_MAX - 1)
    {
        presentation.failed = true;
        Log(LogLevel::Error, localization.GetText("D3D12.FenceExhausted"));
        return false;
    }
    const UINT index = presentation.swapChain->GetCurrentBackBufferIndex();
    if (index >= D3D12Presentation::bufferCount || !presentation.backBuffers[index])
    {
        Log(LogLevel::Error, localization.GetText("D3D12.ClearInputsInvalid"));
        return false;
    }
    if (capture && !ValidCapture(presentation, index, *capture))
    {
        Log(LogLevel::Error, localization.GetText("D3D12.CaptureInputsInvalid"));
        return false;
    }
    if(capture&&!CheckGraphicsResult(presentation.states->Register(*capture->readback,D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12TrackedHeap::Readback,"FrameCapture"),localization,"D3D12.StateTrackingFailed"))return false;
    auto& frame = presentation.frames[presentation.frameCursor];
    if(!BeginD3D12FrameCommands(presentation,localization))return false;
    std::optional<se::CpuScope> recording;recording.emplace(presentation.cpuProfiler,"CPU.CommandRecord");
    const bool pbr=content.meshPipeline&&se::rhi::UsesLitLayout(content.meshPipeline->prepared->program->source.kind);
    if(shadow&&!PrepareD3D12Shadow(presentation,frame,content,localization)){presentation.commandList->Close();return false;}
    if(forward&&!PrepareD3D12Forward(presentation,frame,content,localization)){presentation.commandList->Close();return false;}
    if(post&&!PrepareD3D12PostProcess(presentation,frame,content,localization)){presentation.commandList->Close();return false;}
    D3D12GuiUpload guiUpload,gameUiUpload;
    if(gui&&!PrepareD3D12Gui(presentation,frame,gui->layer,frame.guiDescriptors,guiUpload,localization)){presentation.commandList->Close();return false;}
    if(content.gameUi)
    {
        if(!PrepareD3D12Gui(presentation,frame,*content.gameUi,frame.gameUiDescriptors,gameUiUpload,localization)){presentation.commandList->Close();return false;}
        if(!gui&&!presentation.gameUiRtvs)
        {
            ComPtr<ID3D12Device> device;
            if(FAILED(presentation.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))
                ||!CheckGraphicsResult(presentation.rtvPool->Allocate(presentation.bufferCount,presentation.gameUiRtvs),localization,"D3D12.DescriptorCapacityExceeded")){presentation.commandList->Close();return false;}
            D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            for(UINT i=0;i<presentation.bufferCount;++i)device->CreateRenderTargetView(presentation.backBuffers[i].Get(),&view,presentation.gameUiRtvs->Cpu(i));
        }
    }
    if(content.hdrCapture&&(!ValidCapture(presentation,index,*content.hdrCapture,true)
        ||!CheckGraphicsResult(presentation.states->Register(*content.hdrCapture->readback,D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"HdrCapture"),localization,"D3D12.StateTrackingFailed")))
    {presentation.commandList->Close();return false;}
    if(content.shadowCapture&&(!ValidCapture(presentation,index,*content.shadowCapture,false,frame.shadow->depth.Get())
        ||!CheckGraphicsResult(presentation.states->Register(*content.shadowCapture->readback,D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"ShadowCapture"),localization,"D3D12.StateTrackingFailed")))
    {presentation.commandList->Close();return false;}
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> textureViews;textureViews.reserve(content.meshes.size());
    for(const auto& draw:content.meshes)
    {
        textureViews.push_back(draw.mesh->textureViews->Cpu(draw.textureIsData?1:0));
        if(pbr)for(const auto& texture:draw.mesh->pbrTextures)textureViews.push_back(texture.views->Cpu(1));
        if(shadow)textureViews.push_back(frame.shadow->srv->Cpu());
        if(content.environment)for(const auto* texture:content.environmentTextures)textureViews.push_back(texture->views->Cpu(1));
    }
    ComPtr<ID3D12Device> device;
    if(!CheckGraphicsResult(presentation.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())),localization,"D3D12.DescriptorCapacityExceeded")
        ||!CheckGraphicsResult(frame.descriptors.Prepare(*device.Get(),textureViews,content.environment?7:shadow?4:pbr?3:1),localization,"D3D12.DescriptorCapacityExceeded"))
    {presentation.commandList->Close();return false;}
    frame.drawConstants.reserve(content.meshes.size());
    frame.instanceSlices.reserve(content.meshes.size());
    presentation.frameInstances=presentation.frameInstanceBytes=presentation.frameDrawConstantBytes=0;
    for(size_t draw=0;draw<content.meshes.size();++draw)
    {
        D3D12UploadSlice slice;
        auto& constants=presentation.dedicatedUploads?frame.baselineConstants:frame.uploads;
        if(!CheckGraphicsResult(constants.Allocate(pbr?se::rhi::PbrConstantStride:D3D12MeshConstantStride,256,slice),localization,"D3D12.UploadCapacityExceeded"))
        {presentation.commandList->Close();return false;}
        frame.drawConstants.push_back(slice);
    }
    presentation.frameDrawConstantBytes=UINT64(content.meshes.size())*(pbr?se::rhi::PbrConstantStride:D3D12MeshConstantStride);
    // 此子区段已包含在CommandRecord内；成本报告不可重复相加。
    std::optional<se::CpuScope> instancePacking;instancePacking.emplace(presentation.cpuProfiler,"CPU.InstancePacking");
    for(const auto& draw:content.meshes)
    {
        D3D12UploadSlice slice;
        if(!draw.instances.empty())
        {
            const UINT64 bytes=UINT64(draw.instances.size())*se::rhi::InstanceGpuStride;
            if(!CheckGraphicsResult(frame.uploads.Allocate(bytes,16,slice),localization,"D3D12.UploadCapacityExceeded"))
            {presentation.commandList->Close();return false;}
            for(size_t instanceIndex=0;instanceIndex<draw.instances.size();++instanceIndex)
            {
                const auto& p=draw.instances[instanceIndex];const auto& world=p.world;
                const se::Vec3 x{world(0,0),world(1,0),world(2,0)},y{world(0,1),world(1,1),world(2,1)},z{world(0,2),world(1,2),world(2,2)};
                const float sign=se::Dot(se::Cross(x,y),z)<0?-1.0f:1.0f;
                const se::rhi::InstanceGpuData data{clipFromWorld*world,world,se::Transpose(*se::Inverse(world)),p.tint,
                    {p.metallic,p.roughness,p.normalScale,sign},{p.normalMapping?1.0f:0.0f,p.unlit?1.0f:0.0f,p.doubleSided?1.0f:0.0f,p.receivesShadow?1.0f:0.0f}};
                std::memcpy(slice.cpu+instanceIndex*se::rhi::InstanceGpuStride,&data,sizeof(data));
            }
            presentation.frameInstances+=draw.instances.size();presentation.frameInstanceBytes+=bytes;
        }
        frame.instanceSlices.push_back(slice);
    }
    instancePacking.reset();
    std::vector<D3D12IndirectPrepared> indirectPrepared(indirects.size());
    std::vector<D3D12IndirectOcclusion> indirectOcclusion(indirects.size());
    if(indirect)
    {
        for(size_t batchIndex=0;batchIndex<indirects.size();++batchIndex)
        {
            const auto& batch=indirects[batchIndex];
            if(batch.hiz)
            {
                auto& occlusion=indirectOcclusion[batchIndex];
                occlusion.heap=batch.hiz->descriptors.Get();occlusion.srv=batch.hiz->hzbSrv;
                occlusion.clipFromWorld=clipFromWorld;occlusion.width=sceneWidth;occlusion.height=sceneHeight;
                occlusion.mipCount=batch.hiz->mipCount;occlusion.depthBias=batch.depthBias;
            }
            if(!CheckGraphicsResult(batch.kernel->Prepare(frame.uploads,batch.bounds,batch.frustum,frame.instanceSlices[batchIndex],
                content.meshes[batchIndex].mesh->indexCount,static_cast<UINT>(batch.bounds.size()),indirectPrepared[batchIndex],
                batch.hiz?&indirectOcclusion[batchIndex]:nullptr),localization,"D3D12.IndirectPrepareFailed"))
            {presentation.commandList->Close();return false;}
        }
    }
    if(presentation.timingEnabled)presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);

    D3D12StateRecording states(*presentation.states);
    const auto use=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES usage,const char* pass){
        if(!resource||!CheckGraphicsResult(states.Use(*presentation.commandList.Get(),*resource,usage,pass),localization,"D3D12.StateTrackingFailed"))
        {presentation.commandList->Close();presentation.failed=true;return false;}
        return true;
    };
    if(!use(presentation.backBuffers[index].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"Clear"))return false;
    if(forward&&content.sceneHdr){if(!use(content.sceneHdr->color.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"ClearSceneHDR"))return false;}
    else if(forward&&!use(presentation.hdr.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"ClearHDR"))return false;
    if(gui&&!content.sceneHdr&&!use(gui->scene->color.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"ClearScene"))return false;
    if(sceneDepth&&!use(sceneDepth,D3D12_RESOURCE_STATE_DEPTH_WRITE,"ClearDepth"))return false;
    if(triangle&&!use(triangle->vertices.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Triangle"))return false;
    if(forward&&!use(frame.forwardConstants.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"ForwardConstants"))return false;
    if(content.environment)for(const auto* texture:content.environmentTextures)
        if(!use(texture->resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Environment"))return false;
    if(content.colorLut&&!use(content.colorLut->resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"OutputLut"))return false;
    if(content.meshPipeline)
    {
        for(const auto& slice:frame.drawConstants)
            if(!use(slice.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"OpaqueConstants"))return false;
        for(const auto& slice:frame.instanceSlices)if(slice.resource)
            if(!use(slice.resource,D3D12_RESOURCE_STATE_GENERIC_READ,"InstanceData"))return false;
        for(const auto& draw:content.meshes)
        {
            if(!use(draw.mesh->vertices.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"OpaqueVertices")
                ||!use(draw.mesh->indices.Get(),D3D12_RESOURCE_STATE_INDEX_BUFFER,"OpaqueIndices")
                ||!use(draw.mesh->texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"OpaqueTexture"))return false;
            if(pbr)for(const auto& texture:draw.mesh->pbrTextures)
                if(!use(texture.resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"PbrTexture"))return false;
        }
    }

    const auto rtv=forward?(content.sceneHdr?content.sceneHdr->rtv->Cpu():presentation.hdrRtv->Cpu()):gui?gui->scene->rtv->Cpu():presentation.rtvs->Cpu(index);
    constexpr float clearColor[] = { 0.08f, 0.18f, 0.32f, 1.0f }; // 深蓝色 RGBA。
    constexpr float zeroColor[]{0,0,0,1};
    presentation.commandList->ClearRenderTargetView(rtv, forward?zeroColor:clearColor, 0, nullptr);
    if(sceneDepth&&sceneDsv)
        presentation.commandList->ClearDepthStencilView(sceneDsv->Cpu(),D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    if(presentation.timingEnabled)presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
    if(shadow)
    {
        if(presentation.timingEnabled)presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2);
        if(!RecordD3D12Shadow(presentation,frame,content,states,localization)){presentation.commandList->Close();return false;}
        if(presentation.timingEnabled)presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,3);
        if(content.shadowCapture)
        {
            if(!use(frame.shadow->depth.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"ShadowCapture")
                ||!use(content.shadowCapture->readback,D3D12_RESOURCE_STATE_COPY_DEST,"ShadowCapture"))return false;
            D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=content.shadowCapture->readback;destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=content.shadowCapture->footprint;
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=frame.shadow->depth.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            presentation.commandList->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        }
        if(!use(frame.shadow->depth.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"ShadowSample"))return false;
    }
    if(indirect&&indirects.front().hiz)
    {
        // G06.10：多批次共用一次 HZB；prepass 逐批次写入同一深度目标。
        // G06.11：telemetry 的 0/1 查询包住整个 prepass。
        if(telemetry)presentation.commandList->EndQuery(content.occlusionTelemetry->queries,D3D12_QUERY_TYPE_TIMESTAMP,0);
        const auto dsv=sceneDsv->Cpu();
        presentation.commandList->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
        for(size_t batchIndex=0;batchIndex<indirects.size();++batchIndex)
            indirects[batchIndex].prepass->Record(*presentation.commandList.Get(),*content.meshes[batchIndex].mesh,
                frame.instanceSlices[batchIndex].Gpu(),sceneWidth,sceneHeight,static_cast<UINT>(indirects[batchIndex].bounds.size()));
        if(telemetry)presentation.commandList->EndQuery(content.occlusionTelemetry->queries,D3D12_QUERY_TYPE_TIMESTAMP,1);
        if(!use(sceneDepth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"HizDepth"))return false;
        if(!CheckGraphicsResult(indirects.front().hizKernel->Record(*presentation.commandList.Get(),states,*indirects.front().hiz,
            {indirects.front().hizQueries,indirects.front().hizQueryCapacity},indirects.front().hizFirstQuery),
            localization,"D3D12.HiZRecordFailed"))
        {presentation.commandList->Close();presentation.failed=true;return false;}
        if(!use(sceneDepth,D3D12_RESOURCE_STATE_DEPTH_WRITE,"HizToMain"))return false;
    }
    if(forward)
    {
        if(presentation.timingEnabled)presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,shadow?4:2);
        presentation.commandList->OMSetRenderTargets(1,&rtv,FALSE,nullptr);RecordD3D12ForwardSky(presentation,frame,content,sceneWidth,sceneHeight);
        if(presentation.timingEnabled)
        {
            presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,shadow?5:3);
            presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,shadow?6:4);
        }
    }
    std::vector<D3D12IndirectCommand> indirectCommands;
    if(indirect)
    {
        indirectCommands.resize(indirects.size());
        for(size_t batchIndex=0;batchIndex<indirects.size();++batchIndex)
        {
            const auto& batch=indirects[batchIndex];
            if(!CheckGraphicsResult(batch.kernel->Record(*presentation.commandList.Get(),states,indirectPrepared[batchIndex],*batch.output,
                batch.timestamps,batch.timestampFirstQuery,batch.hiz?&indirectOcclusion[batchIndex]:nullptr),localization,"D3D12.IndirectRecordFailed"))
            {presentation.commandList->Close();presentation.failed=true;return false;}
            if(!use(batch.output->instances.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indirect.InstanceRead")
                ||!use(batch.output->arguments.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,"Indirect.ArgumentRead"))return false;
            frame.instanceSlices[batchIndex]={batch.output->instances.Get(),nullptr,0,
                UINT64{batch.bounds.size()}*se::rhi::InstanceGpuStride};
            indirectCommands[batchIndex]=batch.kernel->Command(*batch.output);
        }
    }
    presentation.opaqueBindings={};
    if(content.meshPipeline)
    {
        const auto dsv=sceneDsv->Cpu();
        presentation.commandList->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
        RecordD3D12Meshes(*content.meshPipeline,content.meshes,clipFromWorld,frame,*presentation.commandList.Get(),
            sceneWidth,sceneHeight,content.cacheOpaqueBindings,&presentation.opaqueBindings,indirectCommands);
    }
    else presentation.commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (triangle) RecordD3D12Triangle(*triangle, *presentation.commandList.Get(),
        sceneWidth, sceneHeight, clipFromWorld);
    // G06.09/G06.10：ExecuteIndirect 之后复制参数，供 RHI 在 Fence 完成后异步读取可见数；
    // 多批次共享一块 READBACK 并按 offset 分区，不恢复 INDIRECT_ARGUMENT（下一帧 Record 会显式转 UAV）。
    if(indirect)
    {
        for(const auto& batch:indirects)
        {
            if(!batch.countReadback)continue;
            if(!use(batch.output->arguments.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"Indirect.CountReadback.Source")
                ||!use(batch.countReadback,D3D12_RESOURCE_STATE_COPY_DEST,"Indirect.CountReadback.Target"))return false;
            presentation.commandList->CopyBufferRegion(batch.countReadback,batch.countReadbackOffset,
                batch.output->arguments.Get(),0,D3D12Indirect::argumentBytes);
        }
    }
    if(indirect&&indirects.front().timestamps.heap)
        presentation.commandList->EndQuery(indirects.front().timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,telemetry?5u:4u);
    if(presentation.timingEnabled)presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,shadow?7:forward?5:2);
    if(forward)
    {
        if(content.hdrCapture)
        {
            auto* hdrSource=content.sceneHdr?content.sceneHdr->color.resource.Get():presentation.hdr.Get();
            if(!use(hdrSource,D3D12_RESOURCE_STATE_COPY_SOURCE,"HdrCapture")
                ||!use(content.hdrCapture->readback,D3D12_RESOURCE_STATE_COPY_DEST,"HdrCapture"))return false;
            D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=content.hdrCapture->readback;
            destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=content.hdrCapture->footprint;
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=hdrSource;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            presentation.commandList->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        }
        if(post)
        {
            if(!RecordD3D12PostProcess(presentation,frame,content,states,index,localization)){presentation.commandList->Close();presentation.failed=true;return false;}
        }
        else
        {
            if(presentation.timingEnabled)presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,shadow?8:6);
            auto* hdrSource=content.sceneHdr?content.sceneHdr->color.resource.Get():presentation.hdr.Get();
            if(!use(hdrSource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Output"))return false;
            const auto outputRtv=content.sceneLdr?content.sceneLdr->rtv->Cpu():presentation.rtvs->Cpu(index);
            if(content.sceneLdr&&!use(content.sceneLdr->color.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"OutputLdr"))return false;
            presentation.commandList->OMSetRenderTargets(1,&outputRtv,FALSE,nullptr);
            RecordD3D12ForwardOutput(presentation,frame,content,sceneWidth,sceneHeight,outputRtv);
            if(presentation.timingEnabled)presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,shadow?9:7);
        }
    }

    if(content.gameUi)
    {
        auto* target=gui?gui->scene->color.resource.Get():presentation.backBuffers[index].Get();
        if(!use(target,D3D12_RESOURCE_STATE_RENDER_TARGET,"GameUi"))return false;
        const auto targetRtv=gui?gui->scene->srgbRtv->Cpu():presentation.gameUiRtvs->Cpu(index);
        presentation.commandList->OMSetRenderTargets(1,&targetRtv,FALSE,nullptr);
        if(!RecordD3D12GuiLayer(presentation,*content.gameUi,frame.gameUiDescriptors,gameUiUpload,states,sceneWidth,sceneHeight,localization))
        {presentation.commandList->Close();presentation.failed=true;return false;}
    }
    if(gui&&!RecordD3D12Gui(presentation,frame,*gui,guiUpload,states,index,localization))
    {presentation.commandList->Close();presentation.failed=true;return false;}
    if (capture)
    {
        if(!use(presentation.backBuffers[index].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"Capture")
            ||!use(capture->readback,D3D12_RESOURCE_STATE_COPY_DEST,"Capture"))return false;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = capture->readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = capture->footprint;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = presentation.backBuffers[index].Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        presentation.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    if(!use(presentation.backBuffers[index].Get(),D3D12_RESOURCE_STATE_PRESENT,"Present"))return false;
    if(presentation.timingEnabled)
    {
        presentation.commandList->EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,post?(shadow?20:18):shadow?10:forward?8:3);
        if(!use(frame.timestampReadback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"TimestampResolve"))return false;
        presentation.commandList->ResolveQueryData(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,post?(shadow?21:19):shadow?11:forward?9:4,frame.timestampReadback.Get(),0);
    }
    if(indirect&&indirects.front().timestamps.heap)
    {
        if(!use(indirects.front().timestampReadback,D3D12_RESOURCE_STATE_COPY_DEST,"Indirect.TimestampResolve"))return false;
        presentation.commandList->ResolveQueryData(indirects.front().timestamps.heap,D3D12_QUERY_TYPE_TIMESTAMP,0,
            telemetry?telemetryQueries:(indirects.front().hiz?8u:5u),indirects.front().timestampReadback,0);
    }
    if (!CheckGraphicsResult(presentation.commandList->Close(), localization,
        "D3D12.CloseCommandListFailed"))
    {
        presentation.failed = true;
        return false;
    }

    recording.reset();
    se::CpuScope submission(presentation.cpuProfiler,"CPU.SubmitPresent");
    ID3D12CommandList* lists[] = { presentation.commandList.Get() };
    presentation.gpuWorkPending = true;
    presentation.untrackedSubmission = true;
    presentation.queue->ExecuteCommandLists(1, lists);
    states.Commit();
    const HRESULT presentResult = presentation.swapChain->Present(1, 0);
    // Present 失败也要尝试 Signal；不能丢掉已执行命令的寿命标记。
    const bool signaled = CheckGraphicsResult(SignalSubmittedWork(presentation, frame.fenceValue),
        localization, "D3D12.SignalFenceFailed");
    if(presentation.timingEnabled&&signaled)
    {
        frame.timestampsPending=true;frame.timestampSubmission=presentation.submittedFrames+1;
        frame.timestampFrame=content.sourceFrame?content.sourceFrame:frame.timestampSubmission;
        frame.timestampDraws=triangle?1:static_cast<UINT>(content.meshes.size());
        frame.timestampForward=forward;
        frame.timestampShadow=shadow;
        frame.timestampPost=post;frame.timestampBloom=post&&content.post->bloom;frame.timestampToneMapping=post&&content.post->toneMapping;
        frame.timestampFxaa=post&&content.post->fxaa;frame.timestampUi=post&&!content.overlays.empty();
    }
    ++presentation.submittedFrames;
    presentation.frameCursor = (presentation.frameCursor + 1) % D3D12Presentation::frameCount;
    const bool presented = CheckGraphicsResult(presentResult, localization, "D3D12.PresentFailed");
    presentation.occluded = presentResult == DXGI_STATUS_OCCLUDED;
    presentation.failed = !signaled || !presented;
    if (presentation.failed && DeviceWasRemoved(presentation)) ReportDeviceRemoval(presentation, localization);
    return !presentation.failed;
}

bool RenderD3D12Frame(D3D12Presentation& presentation,const Localization& localization,
    const D3D12Triangle* triangle,const D3D12FrameCapture* capture,const se::Mat4& clipFromWorld)
{
    D3D12FrameContent content;content.triangle=triangle;content.viewProjection=clipFromWorld;
    return RenderD3D12Frame(presentation,localization,content,capture);
}

bool RenderD3D12ClearFrame(D3D12Presentation& presentation, const Localization& localization)
{
    return RenderD3D12Frame(presentation, localization);
}

bool WaitForD3D12Gpu(D3D12Presentation& presentation, const Localization& localization,
    D3D12WaitReason reason)
{
    if (!presentation.queue || !presentation.fence || !presentation.fenceEvent.IsValid()
        || presentation.uploadStates || reason >= D3D12WaitReason::Count)
    {
        Log(LogLevel::Error, localization.GetText("D3D12.ClearInputsInvalid"));
        return false;
    }
    if (!CheckGraphicsResult(FlushQueue(presentation, reason), localization, "D3D12.WaitFenceFailed"))
    {
        if (DeviceWasRemoved(presentation)) ReportDeviceRemoval(presentation, localization);
        presentation.failed = true;
        return false;
    }
    return ReadD3D12GpuTimings(presentation,localization);
}

bool ResizeD3D12Presentation(D3D12Presentation& presentation, UINT width, UINT height,
    const Localization& localization)
{
    if (!presentation.swapChain || presentation.failed || presentation.uploadStates)
    {
        Log(LogLevel::Error, localization.GetText("D3D12.ClearInputsInvalid"));
        return false;
    }
    if (!width || !height || (width == presentation.width && height == presentation.height)) return true;
    if (!WaitForD3D12Gpu(presentation, localization, D3D12WaitReason::Resize)) return false;

    // 丢弃共用列表的旧后缓冲引用。所有 allocator 的上次使用均已在上方完成。
    for (auto& frame : presentation.frames)
    {
        if (!CheckGraphicsResult(frame.allocator->Reset(), localization, "D3D12.ResetCommandAllocatorFailed"))
        { presentation.failed = true; return false; }
        frame.fenceValue = 0;
    }
    if (!CheckGraphicsResult(presentation.commandList->Reset(presentation.frames[0].allocator.Get(), nullptr),
        localization, "D3D12.ResetCommandListFailed")
        || !CheckGraphicsResult(presentation.commandList->Close(), localization, "D3D12.CloseCommandListFailed"))
    { presentation.failed = true; return false; }
    for (auto& buffer : presentation.backBuffers) buffer.Reset();
    presentation.depth.Reset();presentation.hdr.Reset();presentation.hdrRtv.reset();presentation.hdrSrv.reset();presentation.post.reset();presentation.gameUiRtvs.reset();
    presentation.width = presentation.height = 0;
    if (!CheckGraphicsResult(presentation.swapChain->ResizeBuffers(D3D12Presentation::bufferCount,
        width, height, D3D12Presentation::bufferFormat, 0), localization, "D3D12.ResizeBuffersFailed"))
    { presentation.failed = true; return false; }

    ComPtr<ID3D12Device> device;
    if (!CheckGraphicsResult(presentation.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())),
        localization, "D3D12.ResizeBuffersFailed"))
    { presentation.failed = true; return false; }
    for (UINT index = 0; index < D3D12Presentation::bufferCount; ++index)
    {
        if (!CheckGraphicsResult(presentation.swapChain->GetBuffer(index,
            IID_PPV_ARGS(presentation.backBuffers[index].GetAddressOf())), localization,
            "D3D12.GetBackBufferFailed"))
        { presentation.failed = true; return false; }
        if(!CheckGraphicsResult(presentation.states->Register(*presentation.backBuffers[index].Get(),D3D12_RESOURCE_STATE_PRESENT,
            D3D12TrackedHeap::Default,"Backbuffer["+std::to_string(index)+"]"),localization,"D3D12.StateTrackingFailed"))return false;
        device->CreateRenderTargetView(presentation.backBuffers[index].Get(), nullptr, presentation.rtvs->Cpu(index));
    }
    if(!CreateDepth(presentation,*device.Get(),width,height,localization))
    {presentation.failed=true;return false;}
    presentation.width = width;
    presentation.height = height;
    presentation.frameCursor = 0;
    presentation.occluded = false;
    return true;
}

void ReleaseD3D12Presentation(D3D12Presentation& presentation) noexcept
{
    // 未提交批次只取消候选状态；已提交工作仍按原来的 Fence 收拢。
    presentation.uploadStates.reset();
    if (presentation.gpuWorkPending && !DeviceWasRemoved(presentation)
        && FAILED(FlushQueue(presentation, D3D12WaitReason::Shutdown)) && !DeviceWasRemoved(presentation))
    {
        // 语言表/日志设施可能已经退出；只在无法安全释放时使用独立底层回退。
        std::fputs("D3D12 shutdown could not prove GPU completion; refusing to release in-use resources.\n", stderr);
        std::terminate();
    }
    presentation.commandList.Reset();
    for (auto& frame : presentation.frames)
    {
        frame.allocator.Reset();frame.fenceValue=0;
        frame.timestampQueries.Reset();frame.timestampReadback.Reset();frame.timestampsPending=false;
        frame.drawConstants.clear();frame.instanceSlices.clear();frame.uploadDestinations.clear();frame.uploads.Release();frame.baselineConstants.Release();frame.descriptors.Release();frame.outputDescriptors.Release();frame.environmentDescriptors.Release();frame.postDescriptors.Release();frame.guiDescriptors.Release();frame.gameUiDescriptors.Release();frame.forwardConstants={};frame.shadow.reset();frame.constantCursor=0;
    }
    presentation.fenceEvent.Close();
    presentation.fence.Reset();
    for (auto& buffer : presentation.backBuffers) buffer.Reset();
    presentation.rtvs.reset();presentation.gameUiRtvs.reset();
    presentation.depth.Reset();
    presentation.dsv.reset();
    presentation.hdr.Reset();presentation.hdrRtv.reset();presentation.hdrSrv.reset();presentation.post.reset();presentation.srvPool.reset();presentation.rtvPool.reset();
    presentation.shadow.reset();presentation.shadowTargets.clear();presentation.dsvPool.reset();
    presentation.swapChain.Reset();
    if(presentation.states)presentation.states->UnbindQueue(presentation.queue.Get());
    presentation.queue.Reset();presentation.states.reset();presentation.memory.reset();
    presentation.width = 0;
    presentation.height = 0;
    presentation.nextFenceValue = 1;
    presentation.frameCursor = 0;
    presentation.gpuWorkPending = false;
    presentation.untrackedSubmission = false;
    presentation.failed = false;
    presentation.occluded = false;
    presentation.timingEnabled=false;presentation.timestampFrequency=0;presentation.cpuProfiler=nullptr;
    presentation.timings.clear();presentation.droppedTimingSamples=0;
}

bool WaitD3D12Submission(D3D12Presentation& p,UINT64 fence,const Localization& text,D3D12WaitReason reason)
{
    if(!p.fence||!p.fenceEvent.IsValid()||!fence)return false;
    if(!CheckGraphicsResult(WaitForFence(p,fence,reason),text,"D3D12.WaitFenceFailed"))
    {p.failed=true;if(DeviceWasRemoved(p))ReportDeviceRemoval(p,text);return false;}
    return ReadD3D12GpuTimings(p,text);
}
void CollectD3D12UploadReferences(D3D12Presentation& p)
{
    if(!p.fence)return;const auto completed=p.fence->GetCompletedValue();if(completed==UINT64_MAX)return;
    for(UINT index=0;index<p.frameCount;++index)
        if((!p.uploadStates||index!=p.frameCursor)&&p.frames[index].fenceValue<=completed)
        {p.frames[index].uploadDestinations.clear();p.frames[index].shadow.reset();}
}
bool BeginD3D12FrameCommands(D3D12Presentation& p,const Localization& text)
{
    if(p.failed||p.uploadStates||!p.queue||!p.states||!p.commandList||!p.fence||p.frameCursor>=p.frameCount
        ||p.nextFenceValue>=UINT64_MAX-1)return false;
    auto& frame=p.frames[p.frameCursor];
    // 先等待自己上一次的 Fence，再回收页和目标引用；不 drain 其他在途帧。
    if(!CheckGraphicsResult(WaitForFence(p,frame.fenceValue,D3D12WaitReason::FrameReuse),text,"D3D12.WaitFenceFailed")
        ||!ReadD3D12GpuTimings(p,text)
        ||!CheckGraphicsResult(frame.allocator->Reset(),text,"D3D12.ResetCommandAllocatorFailed")
        ||!CheckGraphicsResult(p.commandList->Reset(frame.allocator.Get(),nullptr),text,"D3D12.ResetCommandListFailed"))
    {p.failed=true;if(DeviceWasRemoved(p))ReportDeviceRemoval(p,text);return false;}
    frame.uploadDestinations.clear();frame.drawConstants.clear();frame.instanceSlices.clear();frame.uploads.Reset();frame.baselineConstants.Reset();frame.descriptors.Reset();frame.outputDescriptors.Reset();frame.environmentDescriptors.Reset();frame.postDescriptors.Reset();frame.guiDescriptors.Reset();frame.gameUiDescriptors.Reset();frame.forwardConstants={};frame.shadow.reset();frame.constantCursor=0;
    CollectD3D12UploadReferences(p);return true;
}
bool BeginD3D12UploadBatch(D3D12Presentation& p,const Localization& text)
{
    if(!BeginD3D12FrameCommands(p,text))return false;
    p.uploadStates=std::make_unique<D3D12StateRecording>(*p.states);return true;
}
bool EndD3D12UploadBatch(D3D12Presentation& p,UINT64& fence,const Localization& text)
{
    fence=0;if(!p.uploadStates||p.failed)return false;
    if(!CheckGraphicsResult(p.commandList->Close(),text,"D3D12.CloseCommandListFailed")){p.failed=true;return false;}
    ID3D12CommandList* lists[]{p.commandList.Get()};p.gpuWorkPending=p.untrackedSubmission=true;
    p.queue->ExecuteCommandLists(1,lists);p.uploadStates->Commit();p.uploadStates.reset();
    auto& frame=p.frames[p.frameCursor];
    const auto success=CheckGraphicsResult(SignalSubmittedWork(p,frame.fenceValue),text,"D3D12.SignalFenceFailed");
    ++p.uploadSubmissions;fence=frame.fenceValue;p.frameCursor=(p.frameCursor+1)%p.frameCount;
    if(!success)p.failed=true;return success;
}
