#pragma once
#include <cstdint>
#include <vector>

namespace se
{
// G05.13：HLOD 代理显存策略。只做预算/每帧上传上限/最久未用淘汰的决策，
// 代理的 GPU 资源仍由调用方持有并在淘汰时释放；不引入第二套资源所有权。
class HlodProxyCache
{
public:
    struct Statistics
    {
        std::uint64_t bytes=0,peakBytes=0,uploadedBytes=0;
        std::uint32_t uploads=0,evictions=0,frames=0;
    };
    // budgetBytes==0 表示不限预算；uploadsPerFrame==0 表示不限每帧上传数。
    void Configure(std::uint64_t budgetBytes,std::uint32_t uploadsPerFrame);
    void Reset();
    void BeginFrame();
    bool CanUpload() const;
    bool Fits(std::uint64_t bytes) const;
    void AddUpload(std::uint64_t bytes);
    void Touch(std::uint32_t cell);
    void Insert(std::uint32_t cell,std::uint64_t bytes);
    void Erase(std::uint32_t cell);
    bool Present(std::uint32_t cell) const;
    // 只考虑本帧未选中、已存在的代理；返回最久未用者。
    bool SelectEviction(std::uint32_t& cell) const;
    bool OverBudget() const;
    std::uint64_t Bytes() const noexcept {return stats_.bytes;}
    std::uint64_t Budget() const noexcept {return budget_;}
    std::uint32_t UploadsPerFrame() const noexcept {return uploadsPerFrame_;}
    std::uint32_t UploadsThisFrame() const noexcept {return uploadsThisFrame_;}
    const Statistics& Stats() const noexcept {return stats_;}
private:
    void Ensure(std::uint32_t cell) const;
    mutable std::vector<std::uint64_t> lastUsed_,bytes_;
    mutable std::vector<char> present_,touched_;
    std::uint64_t budget_=0;
    std::uint32_t uploadsPerFrame_=0,uploadsThisFrame_=0,frame_=0;
    Statistics stats_;
};
}
