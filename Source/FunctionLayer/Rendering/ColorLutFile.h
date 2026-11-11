#pragma once
#include "ColorGrading.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace se
{
// G15：`.selut` 资产热重载状态。revision 只在成功读取并替换数据时递增；
// 失败时保留最近一次成功的 LUT，只更新 error/failures，下一次文件变化再重试。
struct ColorLutFileStatus
{
    bool loaded=false;
    std::uint64_t revision=0,reloads=0,failures=0;
    std::uint32_t size=0;
    ColorLutColorSpace colorSpace=ColorLutColorSpace::Linear;
    std::string error,detail;
};

// G15：编辑器选中的 `.selut` 在运行时热重载。轮询只比较修改时间与文件大小，
// 不逐帧读盘；文件被删除/写坏时继续使用旧数据，避免画面跳回未调色路径。
class ColorLutFile
{
public:
    using Clock=std::chrono::steady_clock;
    explicit ColorLutFile(float pollIntervalSeconds=0.20f) noexcept;
    // 立即读取目标文件；失败时清空旧数据并返回 false，error 为稳定文本键。
    bool Open(const std::filesystem::path& path,std::string& error);
    void Close() noexcept;
    // 到达轮询间隔才检查文件；force 供手动重载与测试使用。
    // 返回 true 表示本次成功读取并替换了晶格；写坏/缺失时返回 false，但状态会更新。
    bool Poll(bool force=false);
    void SetPollInterval(float seconds) noexcept;
    bool Ready() const noexcept{return status_.loaded;}
    const ColorLut& Lut() const noexcept{return lut_;}
    const ColorLutFileStatus& Status() const noexcept{return status_;}
    // 供 RHI 后处理按 revision 上传；重载时整体替换，不修改正在使用的旧数组。
    std::shared_ptr<const std::vector<Vec3>> Values() const noexcept{return values_;}
    const std::filesystem::path& Path() const noexcept{return path_;}
private:
    bool Reload(bool force);
    std::filesystem::path path_;
    ColorLut lut_;
    std::shared_ptr<const std::vector<Vec3>> values_;
    ColorLutFileStatus status_;
    float pollInterval_=0.20f;
    Clock::time_point lastPoll_{};
    std::filesystem::file_time_type stampTime_{};
    std::uintmax_t stampSize_=0;
    bool haveStamp_=false;
};
}
