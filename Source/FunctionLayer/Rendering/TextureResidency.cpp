#include "TextureResidency.h"
#include <algorithm>

namespace se
{
std::uint64_t TextureResidency::MipBytes(std::uint64_t baseBytes,std::uint32_t mip,std::uint64_t minimumBytes) noexcept
{
    if(!baseBytes)return 0;
    const std::uint64_t shifted=baseBytes>>(std::min)(mip*2u,63u);
    return (std::max)(shifted,minimumBytes?minimumBytes:std::uint64_t{1});
}

void TextureResidency::Recompute(TextureResidencyEntry& entry) noexcept
{
    loadedBytes_=(loadedBytes_>=entry.loadedBytes)?loadedBytes_-entry.loadedBytes:0;
    entry.loadedBytes=0;
    for(std::uint32_t mip=entry.desiredMip;mip<entry.mipCount;++mip)
        entry.loadedBytes+=MipBytes(entry.baseBytes,mip,entry.minimumBytes);
    loadedBytes_+=entry.loadedBytes;
}

bool TextureResidency::Request(std::uint64_t id,std::uint32_t mipCount,std::uint32_t desiredMip,
    std::uint64_t baseBytes,std::uint64_t frame,std::uint64_t minimumBytes)
{
    if(!mipCount)return false;
    desiredMip=(std::min)(desiredMip,mipCount-1);
    auto& entry=entries_[id];
    entry.id=id;entry.mipCount=mipCount;entry.desiredMip=desiredMip;
    entry.baseBytes=baseBytes;entry.minimumBytes=minimumBytes?minimumBytes:1;entry.lastUsedFrame=frame;
    Recompute(entry);
    Trim();
    const auto* current=Find(id);
    return current&&current->desiredMip<=desiredMip;
}

std::size_t TextureResidency::TrimToBudget()
{
    std::map<std::uint64_t,std::uint32_t> before;
    for(const auto& [id,entry]:entries_)before.emplace(id,entry.desiredMip);
    Trim();
    std::size_t changed=0;
    for(const auto& [id,entry]:entries_)
    {
        const auto previous=before.find(id);
        if(previous==before.end()||previous->second!=entry.desiredMip)++changed;
    }
    return changed;
}
bool TextureResidency::SetDesired(std::uint64_t id,std::uint32_t desiredMip)
{
    const auto found=entries_.find(id);
    if(found==entries_.end())return false;
    auto& entry=found->second;
    desiredMip=(std::min)(desiredMip,entry.mipCount-1);
    if(desiredMip==entry.desiredMip)return false;
    entry.desiredMip=desiredMip;
    Recompute(entry);
    Trim();
    return true;
}

void TextureResidency::Touch(std::uint64_t id,std::uint64_t frame,std::uint32_t desiredMip)
{
    const auto found=entries_.find(id);
    if(found==entries_.end())return;
    auto& entry=found->second;
    entry.lastUsedFrame=frame;
    if(desiredMip==UINT32_MAX||desiredMip>=entry.mipCount)return;
    if(desiredMip==entry.desiredMip)return;
    entry.desiredMip=desiredMip;
    Recompute(entry);
    Trim();
}

void TextureResidency::Trim()
{
    while(loadedBytes_>budget_)
    {
        TextureResidencyEntry* victim=nullptr;
        for(auto& [_,entry]:entries_)
            if(entry.desiredMip+1<entry.mipCount&&(!victim||entry.lastUsedFrame<victim->lastUsedFrame))
                victim=&entry;
        if(!victim)break;
        ++victim->desiredMip;
        Recompute(*victim);
    }
    // 只剩最后一 mip 仍超预算时，保留最小可用表示，交由上层报告预算不足。
    if(loadedBytes_>budget_)
    {
        for(auto iterator=entries_.begin();iterator!=entries_.end();)
            if(iterator->second.loadedBytes==0)iterator=entries_.erase(iterator);
            else ++iterator;
    }
}

const TextureResidencyEntry* TextureResidency::Find(std::uint64_t id) const
{
    const auto found=entries_.find(id);
    return found==entries_.end()?nullptr:&found->second;
}
}
