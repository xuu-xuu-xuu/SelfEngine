#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace se
{
// G13：轻量 Frame Graph。
// 每帧 Reset 后按声明顺序登记 Pass 与资源读写；Compile 负责校验、剔除、拓扑排序、
// 生存期与别名，并给出确定的执行顺序。Graph 只产出“资源 + 用途 + 顺序”，
// 原生子资源状态与屏障仍由 RHI 后端唯一生成，这里不持有 D3D12 状态表。
inline constexpr std::uint32_t FrameGraphInvalid = UINT32_MAX;

struct FrameGraphTextureDesc
{
    std::uint32_t width=0,height=0,mipLevels=1;
    rhi::TextureFormat format=rhi::TextureFormat::Rgba8;
    bool depth=false; // D32 深度目标；format 只用于诊断/分组，深度资源按 4 字节/像素计。
    bool operator==(const FrameGraphTextureDesc&) const=default;
};

struct FrameGraphResourceRef
{
    std::uint32_t resource=FrameGraphInvalid;
    std::uint32_t version=0; // 0 = 外部资源的初始内容；>=1 由 Write 生成。
};

struct FrameGraphPassDesc
{
    std::string_view name;
    rhi::PassKind kind=rhi::PassKind::Clear;
    rhi::ResourceUse color=rhi::ResourceUse::ColorWrite;
    rhi::ResourceUse depth=rhi::ResourceUse::DepthWrite;
    rhi::ResourceUse input=rhi::ResourceUse::None;
    bool cacheBindings=false;
    bool sideEffect=false; // 呈现、清屏副作用或写外部资源之外的“必须有输出”的 Pass。
    std::uint32_t execute=0; // 执行体编号，由使用方解释；Graph 不调用回调。
};

enum class FrameGraphStatus : std::uint8_t
{
    Success=0,
    InvalidArgument,
    DuplicatePass,
    DuplicateWrite,
    UndefinedRead,
    Cycle,
    CapacityExceeded
};

struct FrameGraphStats
{
    std::uint32_t declaredPasses=0,executedPasses=0,culledPasses=0,disabledPasses=0;
    std::uint32_t edges=0,aliasGroups=0;
    std::uint64_t transientBytes=0,aliasedBytes=0,savedBytes=0;
    bool clipped=false; // 只跑前 N 个 Pass 的调试模式截断过顺序。
    double compileMilliseconds=0;
};

struct FrameGraphPassView
{
    std::string_view name;
    rhi::PassKind kind=rhi::PassKind::Clear;
    rhi::ResourceUse color=rhi::ResourceUse::ColorWrite;
    rhi::ResourceUse depth=rhi::ResourceUse::DepthWrite;
    rhi::ResourceUse input=rhi::ResourceUse::None;
    bool cacheBindings=false;
    std::uint32_t execute=0,declaration=0;
    std::span<const FrameGraphResourceRef> reads;  // 已解析到实际生产版本（禁用 Pass 会回退到更早版本）。
    std::span<const FrameGraphResourceRef> writes;
};

struct FrameGraphTextureView
{
    std::string_view name;
    FrameGraphTextureDesc desc;
    bool imported=false;
    std::uint32_t firstPass=FrameGraphInvalid,lastPass=FrameGraphInvalid,aliasGroup=FrameGraphInvalid;
    std::uint64_t bytes=0;
};

class FrameGraph
{
public:
    explicit FrameGraph(std::uint32_t maxPasses=128,std::uint32_t maxResources=192,
        std::uint32_t maxEdges=2048,std::uint32_t maxVersions=16);
    ~FrameGraph();
    FrameGraph(const FrameGraph&)=delete;
    FrameGraph& operator=(const FrameGraph&)=delete;
    FrameGraph(FrameGraph&&) noexcept;
    FrameGraph& operator=(FrameGraph&&) noexcept;

    // 新帧：清空声明与上一帧的编译结果；固定容量的帧 arena 从头复用。
    void Reset() noexcept;

    std::uint32_t AddTexture(std::string_view name,const FrameGraphTextureDesc& desc,bool imported=false);
    std::uint32_t AddPass(const FrameGraphPassDesc& desc);
    // 读取当前最新版本；外部资源在首次写入前版本为 0。
    void Read(std::uint32_t pass,std::uint32_t resource);
    // 读取指定版本；允许指向稍后声明的 Pass，用于显式数据流/环检测。
    void ReadVersion(std::uint32_t pass,FrameGraphResourceRef reference);
    // 写入并生成下一个版本；同一 Pass 对同一资源重复写入会在 Compile 报错。
    FrameGraphResourceRef Write(std::uint32_t pass,std::uint32_t resource);
    void SetPassEnabled(std::uint32_t pass,bool enabled) noexcept;
    // 调试：只保留排序后的前 N 个 Pass（0 表示不截断）。
    void SetPassLimit(std::uint32_t limit) noexcept;
    std::uint32_t LatestVersion(std::uint32_t resource) const noexcept;

    FrameGraphStatus Compile() noexcept;
    std::string_view LastError() const noexcept;
    std::span<const FrameGraphPassView> Passes() const noexcept;      // 编译后的执行顺序。
    std::span<const FrameGraphTextureView> Textures() const noexcept; // 声明顺序。
    std::span<const std::uint32_t> ExecutionOrder() const noexcept;   // Pass 声明下标。
    const FrameGraphStats& Stats() const noexcept;

    bool WriteDot(const std::filesystem::path& path) const;
    bool WriteJson(const std::filesystem::path& path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
