#include "WindowProfile.h"
#include "CoreLayer/Utf.h"
#include <nlohmann/json.hpp>
#include <iomanip>
#include <locale>
#include <sstream>

namespace se
{
namespace
{
std::string ObjectName(HANDLE object)
{
    wchar_t buffer[256]{};DWORD bytes=0;
    if(!GetUserObjectInformationW(object,UOI_NAME,buffer,sizeof(buffer),&bytes))return {};
    std::u32string decoded;std::string encoded;
    const std::wstring_view wide(buffer);
    const std::u16string utf16(wide.begin(),wide.end());
    if(!DecodeUtf16(utf16,decoded)||!EncodeUtf8(decoded,encoded))return {};
    return encoded;
}
}
WindowProfileRenderer::WindowProfileRenderer(SandboxRenderer& renderer,Profiler& profiler,std::filesystem::path directory)
    :renderer_(renderer),profiler_(profiler),directory_(std::move(directory))
{
    rows_.reserve(FrameLimit);
    renderer_.SetProfileOutput(directory_);
    station_=ObjectName(GetProcessWindowStation());
    desktop_=ObjectName(GetThreadDesktop(GetCurrentThreadId()));
}
bool WindowProfileRenderer::Initialize(const RenderSurface& surface,const Localization& localization)
{
    const bool result=renderer_.Initialize(surface,localization);
    adapter_=renderer_.Adapter();failed_=!result;return result;
}
RenderResult WindowProfileRenderer::Render(const RenderFrame& frame,const Localization& localization)
{
    if(rows_.size()>=FrameLimit){overflow_=failed_=true;return RenderResult::Failed;}
    const auto result=renderer_.Render(frame,localization);
    const auto stats=renderer_.Statistics();const auto draws=renderer_.DrawStatistics();const auto ui=renderer_.UiStatistics();
    rows_.push_back({frame.updateIndex,stats.submittedFrames,draws.draws,static_cast<std::uint64_t>(ui.batches),static_cast<std::uint64_t>(ui.quads*4),
        frame.elapsedSeconds,frame.rawDeltaSeconds,frame.updateDeltaSeconds,frame.surface.width,frame.surface.height,
        IsWindowVisible(frame.surface.window)!=FALSE,GetForegroundWindow()==frame.surface.window,result==RenderResult::Occluded,stats});
    if(result==RenderResult::Failed)failed_=true;
    return result;
}
void WindowProfileRenderer::Shutdown() noexcept
{
    if(stopped_)return;stopped_=true;
    try{finalStatistics_=renderer_.Statistics();}catch(...){failed_=true;}
    renderer_.Shutdown();
}
bool WindowProfileRenderer::Finish(bool hostSucceeded)
{
    Shutdown();std::string error;
    std::ostringstream csv;csv.imbue(std::locale::classic());csv<<std::setprecision(17);
    csv<<"frame,submission,elapsed_s,raw_delta_ms,update_delta_ms,width,height,visible,foreground,occluded,draws,ui_draws,ui_vertices,memory_reserved_bytes,memory_used_bytes,upload_reserved_bytes,persistent_descriptors,tracked_resources,global_waits\n";
    for(const auto& row:rows_){const auto& s=row.statistics;
        csv<<row.frame<<','<<row.submission<<','<<row.elapsed<<','<<row.rawDelta*1000<<','<<row.updateDelta*1000<<','
            <<row.width<<','<<row.height<<','<<row.visible<<','<<row.foreground<<','<<row.occluded<<','<<row.draws<<','<<row.uiDraws<<','<<row.uiVertices<<','
            <<s.memoryReservedBytes<<','<<s.memoryUsedBytes<<','<<s.uploadReservedBytes<<','<<s.persistentDescriptors<<','<<s.trackedResources<<','<<s.globalWaits<<'\n';
    }
    bool success=hostSucceeded&&!failed_&&!overflow_&&profiler_.DroppedFrames()==0&&renderer_.ProfileExportSucceeded()&&!rows_.empty();
    success=WriteProfileCsv(directory_/"frames.csv",csv.str(),error)&&success;
    success=profiler_.ExportFramesCsv(directory_/"cpu.csv",error)&&success;
    success=profiler_.ExportSummaryCsv(directory_/"cpu-summary.csv",error)&&success;
    const auto& s=finalStatistics_;
    const nlohmann::json report={{"schema",1},{"success",success},{"frame_limit",FrameLimit},{"overflow",overflow_},
        {"rendered_frames",rows_.size()},{"cpu_frames",profiler_.Frames().size()},{"cpu_dropped_frames",profiler_.DroppedFrames()},
        {"gpu_export_ok",renderer_.ProfileExportSucceeded()},{"window_station",station_},{"thread_desktop",desktop_},
        {"adapter",{{"name",adapter_.name},{"vendor",adapter_.vendorId},{"device",adapter_.deviceId},{"dedicated_bytes",adapter_.dedicatedVideoMemory},{"software",adapter_.software}}},
        {"final",{{"submitted_frames",s.submittedFrames},{"global_waits_before_shutdown",s.globalWaits},{"debug_warnings",s.debugWarnings},{"debug_errors",s.debugErrors},
            {"memory_peak_reserved_bytes",s.memoryPeakReservedBytes},{"memory_peak_used_bytes",s.memoryPeakUsedBytes},{"upload_reserved_bytes",s.uploadReservedBytes},
            {"pipeline_creations",s.pipelineCreations},{"shader_compilations",s.shaderProgramCompilations}}},
        {"memory_definition","D3D12 allocator API allocation bytes; not measured resident VRAM"},
        {"timing_definition","CPU.Frame excludes frame-external Sleep; raw_delta_ms includes time between application timer updates; GPU ticks read after fence"}};
    return WriteProfileCsv(directory_/"window-profile.json",report.dump(2),error)&&success;
}
}
