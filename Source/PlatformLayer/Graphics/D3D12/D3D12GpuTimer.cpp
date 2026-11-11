#include "D3D12GpuTimer.h"
#include "D3D12Presentation.h"
#include "D3D12Result.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Profiler.h"
#include "D3D12StateTracker.h"
#include <cstring>
#include <algorithm>

bool ReadD3D12GpuTimings(D3D12Presentation& p,const Localization& text)
{
    if(!p.fence)return false;
    const auto completed=p.fence->GetCompletedValue();if(completed==UINT64_MAX)return false;
    for(auto& frame:p.frames)
    {
        if(!frame.timestampsPending||!frame.fenceValue||frame.fenceValue>completed)continue;
        se::CpuScope scope(p.cpuProfiler,"CPU.QueryRead");
        se::rhi::GpuTiming sample;sample.sourceFrame=frame.timestampFrame;sample.submission=frame.timestampSubmission;
        sample.frequency=p.timestampFrequency;sample.draws=frame.timestampDraws;
        void* mapped=nullptr;const D3D12_RANGE range{0,frame.timestampPost?sizeof(UINT64)*(frame.timestampShadow?21:19):frame.timestampShadow?sizeof(UINT64)*11:frame.timestampForward?sizeof(UINT64)*9:sizeof(sample.ticks)};
        if(!CheckGraphicsResult(frame.timestampReadback->Map(0,&range,&mapped),text,"D3D12.ReadTimestampsFailed"))return false;
        if(frame.timestampPost)
        {
            std::array<UINT64,21> raw{};std::memcpy(raw.data(),mapped,range.End);const size_t first=frame.timestampShadow?8:6;
            sample.forward=sample.post=true;sample.shadow=frame.timestampShadow;const size_t sky=sample.shadow?4:2,opaque=sky+2;
            sample.passTicks={raw[0],raw[1],raw[sky],raw[sky+1],raw[opaque],raw[opaque+1],raw[first],raw[first+11]};
            if(sample.shadow)sample.shadowTicks={raw[2],raw[3]};std::copy_n(raw.begin()+first,12,sample.postTicks.begin());
            sample.ticks={raw[0],raw[opaque],raw[opaque+1],raw[first+12]};sample.postBloom=frame.timestampBloom;
            sample.postToneMapping=frame.timestampToneMapping;sample.postFxaa=frame.timestampFxaa;sample.postUi=frame.timestampUi;
            for(size_t index=1;index<sample.postTicks.size();++index)if(sample.postTicks[index]<sample.postTicks[index-1])
            {Log(LogLevel::Error,text.GetText("D3D12.InvalidTimestamps"));return false;}
        }
        else if(frame.timestampShadow)
        {
            std::array<UINT64,11> raw{};std::memcpy(raw.data(),mapped,sizeof(raw));sample.forward=sample.shadow=true;
            sample.passTicks={raw[0],raw[1],raw[4],raw[5],raw[6],raw[7],raw[8],raw[9]};sample.shadowTicks={raw[2],raw[3]};
            sample.ticks={raw[0],raw[6],raw[7],raw[10]};
        }
        else if(frame.timestampForward)
        {
            sample.forward=true;std::memcpy(sample.passTicks.data(),mapped,sizeof(sample.passTicks));
            UINT64 end=0;std::memcpy(&end,static_cast<const std::byte*>(mapped)+sizeof(sample.passTicks),sizeof(end));
            sample.ticks={sample.passTicks[0],sample.passTicks[4],sample.passTicks[5],end};
        }
        else std::memcpy(sample.ticks.data(),mapped,sizeof(sample.ticks));
        const D3D12_RANGE noWrites{0,0};frame.timestampReadback->Unmap(0,&noWrites);
        if(sample.forward)for(size_t index=1;index<sample.passTicks.size();++index)if(sample.passTicks[index]<sample.passTicks[index-1])
        {Log(LogLevel::Error,text.GetText("D3D12.InvalidTimestamps"));return false;}
        if(sample.forward&&sample.ticks[3]<sample.passTicks[7]){Log(LogLevel::Error,text.GetText("D3D12.InvalidTimestamps"));return false;}
        if(sample.shadow&&(sample.shadowTicks[0]<sample.passTicks[1]||sample.shadowTicks[1]<sample.shadowTicks[0]||sample.shadowTicks[1]>sample.passTicks[2]))
        {Log(LogLevel::Error,text.GetText("D3D12.InvalidTimestamps"));return false;}
        if(!sample.frequency||sample.ticks[0]>sample.ticks[1]||sample.ticks[1]>sample.ticks[2]||sample.ticks[2]>sample.ticks[3])
        {Log(LogLevel::Error,text.GetText("D3D12.InvalidTimestamps"));return false;}
        sample.frameMilliseconds=double(sample.ticks[3]-sample.ticks[0])*1000.0/double(sample.frequency);
        sample.geometryMilliseconds=double(sample.ticks[2]-sample.ticks[1])*1000.0/double(sample.frequency);
        const auto position=std::upper_bound(p.timings.begin(),p.timings.end(),sample.submission,[](auto submission,const se::rhi::GpuTiming& value){return submission<value.submission;});
        p.timings.insert(position,sample);frame.timestampsPending=false;
        if(p.timings.size()>6000){p.timings.pop_front();++p.droppedTimingSamples;}
    }
    return true;
}
bool SetD3D12GpuTiming(D3D12Presentation& p,bool enabled,const Localization& text)
{
    if(p.timingEnabled==enabled)return true;
    if(!enabled)
    {
        if(!WaitForD3D12Gpu(p,text))return false;
        for(auto& frame:p.frames){frame.timestampQueries.Reset();frame.timestampReadback.Reset();frame.timestampsPending=false;}
        p.timingEnabled=false;p.timestampFrequency=0;return true;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    UINT64 frequency=0;
    if(!p.queue||!CheckGraphicsResult(p.queue->GetTimestampFrequency(&frequency),text,"D3D12.TimestampFrequencyFailed")||!frequency
        ||FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
    struct Queries {Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;Microsoft::WRL::ComPtr<ID3D12Resource> readback;};
    std::array<Queries,D3D12Presentation::frameCount> candidates;
    for(auto& candidate:candidates)
    {
        D3D12_QUERY_HEAP_DESC queries{};queries.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;queries.Count=21;
        if(!CheckGraphicsResult(device->CreateQueryHeap(&queries,IID_PPV_ARGS(candidate.heap.GetAddressOf())),text,"D3D12.CreateTimestampsFailed"))return false;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=sizeof(UINT64)*21;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc={1,0};buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(!CheckGraphicsResult(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(candidate.readback.GetAddressOf())),text,"D3D12.CreateTimestampsFailed"))return false;
        if(!CheckGraphicsResult(p.states->Register(*candidate.readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"GpuTimestamps"),text,"D3D12.StateTrackingFailed"))return false;
    }
    for(size_t i=0;i<candidates.size();++i)
    {p.frames[i].timestampQueries=std::move(candidates[i].heap);p.frames[i].timestampReadback=std::move(candidates[i].readback);}
    p.timestampFrequency=frequency;p.timingEnabled=true;return true;
}
