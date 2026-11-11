#pragma once
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Graphics/D3D12/D3D12StateTracker.h"
#include "PlatformLayer/Graphics/VisibilityData.h"
#include <filesystem>

class Localization;
namespace se
{
struct GpuVisibilityMeasurement
{
    std::uint64_t sequence=0,frequency=0;
    std::array<std::uint64_t,4> ticks{};
    std::uint32_t inputCount=0,capacity=0,total=0,written=0,overflow=0;
    bool diagnostic=false,guards=false;
    double prepareMs=0,recordMs=0,submitMs=0,reuseWaitMs=0,readMs=0,cpuFrameMs=0;
    double setupResetMs=0,dependencyCullMs=0,readbackCopyMs=0;
    std::vector<std::uint32_t> ids;
};
struct GpuVisibilityStatistics
{
    D3D12MemoryStatistics memory;
    D3D12StateStatistics states;
    std::uint64_t uploadReservedBytes=0,readbackAllocationBytes=0;
    std::uint64_t submissions=0,reuseWaits=0,resultReadbacks=0,drains=0;
    std::uint32_t pending=0,resultReadbackResources=0,warnings=0,errors=0;
    bool debugInfoQueue=false,stopped=false;
    double initializationMs=0;
    std::string adapter;
};
// Tool层的两槽实验宿主；单调用线程，正常提交不读可见计数，无Present限帧。
class GpuVisibilityExperiment
{
public:
    GpuVisibilityExperiment(const Localization& text,const std::filesystem::path& shader,bool diagnostic,
        std::uint32_t storageCapacity=MaximumVisibilityItems);
    ~GpuVisibilityExperiment();
    GpuVisibilityExperiment(const GpuVisibilityExperiment&)=delete;
    GpuVisibilityExperiment& operator=(const GpuVisibilityExperiment&)=delete;
    std::uint64_t Submit(std::span<const VisibilityItem> items,const VisibilityFrustum& frustum,std::uint32_t capacity);
    void Drain();
    std::vector<GpuVisibilityMeasurement> TakeResults();
    GpuVisibilityStatistics Statistics() const;
    void Shutdown();
    // 实验测试的借用访问，只用于建立受控Fence门，不另绑队列/状态表。
    ID3D12Device& NativeDevice() const;
    ID3D12CommandQueue& Queue() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
