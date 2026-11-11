#pragma once
#include <cstdint>
#include <map>

namespace se
{
// G04：纹理驻留策略。记录每张纹理已加载的 mip 区间与最近使用帧；
// 只做预算与选择，不负责 IO/上传。
struct TextureResidencyEntry
{
    std::uint64_t id=0;
    std::uint32_t mipCount=1;
    std::uint32_t desiredMip=0; // 当前允许采样的最细 mip
    std::uint64_t baseBytes=0;  // mip0 字节数
    std::uint64_t minimumBytes=1; // 每级最小字节（压缩块 8/16 字节），避免小 mip 被算成 0。
    std::uint64_t lastUsedFrame=0;
    std::uint64_t loadedBytes=0;
};

class TextureResidency
{
public:
    explicit TextureResidency(std::uint64_t budgetBytes=512ull*1024*1024) noexcept : budget_(budgetBytes) {}
    void SetBudget(std::uint64_t budgetBytes) noexcept {budget_=budgetBytes;}
    std::uint64_t Budget() const noexcept {return budget_;}
    std::uint64_t LoadedBytes() const noexcept {return loadedBytes_;}
    // 请求至少驻留 [desiredMip, mipCount) 的 mip；返回是否在预算内完成。
    bool Request(std::uint64_t id,std::uint32_t mipCount,std::uint32_t desiredMip,
        std::uint64_t baseBytes,std::uint64_t frame,std::uint64_t minimumBytes=1);
    void Touch(std::uint64_t id,std::uint64_t frame,std::uint32_t desiredMip=UINT32_MAX);
    // 直接设置目标 mip（用于预算有余量时恢复更细 mip）；内部会按预算 Trim。返回目标是否变化。
    bool SetDesired(std::uint64_t id,std::uint32_t desiredMip);
    // 按“最久未用优先”逐级丢弃最细 mip，直到回到预算内。返回本次被降级的纹理数。
    std::size_t TrimToBudget();
    void Clear() noexcept {entries_.clear();loadedBytes_=0;}
    const TextureResidencyEntry* Find(std::uint64_t id) const;
    const std::map<std::uint64_t,TextureResidencyEntry>& Entries() const noexcept {return entries_;}

private:
    static std::uint64_t MipBytes(std::uint64_t baseBytes,std::uint32_t mip,std::uint64_t minimumBytes=1) noexcept;
    void Recompute(TextureResidencyEntry& entry) noexcept;
    void Trim();

    std::uint64_t budget_=0;
    std::uint64_t loadedBytes_=0;
    std::map<std::uint64_t,TextureResidencyEntry> entries_;
};
}
