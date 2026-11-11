#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace se
{
bool WriteProfileCsv(const std::filesystem::path& path,std::string_view text,std::string& error);
struct CpuSample {std::uint64_t calls=0;double milliseconds=0;};
struct CpuFrame {std::uint64_t frame=0;std::map<std::string,CpuSample> scopes;};
struct CpuStatistics
{
    std::string scope;
    std::size_t frames=0;
    std::uint64_t calls=0;
    double averageMilliseconds=0,p50Milliseconds=0,p95Milliseconds=0,p99Milliseconds=0;
};
class CpuScope;
// CPU 墙钟区段；嵌套区段包含子区段，不可相加解释为总帧时间。实例须活得比 CpuScope 久。
class Profiler
{
public:
    explicit Profiler(std::size_t historyLimit=6000);
    void SetEnabled(bool enabled);
    bool Enabled() const noexcept {return enabled_.load(std::memory_order_relaxed);}
    bool BeginFrame(std::uint64_t frame);
    bool EndFrame();
    void Clear();
    std::vector<CpuFrame> Frames() const;
    std::vector<CpuFrame> FramesSince(std::uint64_t frame) const;
    std::vector<CpuStatistics> Statistics() const;
    std::uint64_t DroppedFrames() const;
    bool ExportFramesCsv(const std::filesystem::path& path,std::string& error) const;
    bool ExportSummaryCsv(const std::filesystem::path& path,std::string& error) const;
    // 接受平台计时器提供的 CPU 区段；也是确定性统计测试入口，不属于 GPU 时间。
    bool RecordCpuDuration(std::string_view scope,double milliseconds,std::uint64_t calls=1);
private:
    friend class CpuScope;
    std::uint64_t Token() const;
    bool Record(std::uint64_t token,std::string_view scope,double milliseconds,std::uint64_t calls);
    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    std::size_t limit_;
    std::uint64_t token_=1,dropped_=0;
    bool active_=false;
    CpuFrame current_;
    std::chrono::steady_clock::time_point started_;
    std::deque<CpuFrame> frames_;
};
class CpuScope
{
public:
    CpuScope(Profiler& profiler,std::string_view name);
    CpuScope(Profiler* profiler,std::string_view name);
    ~CpuScope() noexcept;
    CpuScope(const CpuScope&)=delete;
    CpuScope& operator=(const CpuScope&)=delete;
private:
    Profiler* profiler_=nullptr;
    std::uint64_t token_=0;
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};
}
