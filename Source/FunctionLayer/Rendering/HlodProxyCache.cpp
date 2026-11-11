#include "HlodProxyCache.h"
#include <algorithm>
#include <limits>

namespace se
{
void HlodProxyCache::Configure(std::uint64_t budgetBytes,std::uint32_t uploadsPerFrame)
{
    budget_=budgetBytes;uploadsPerFrame_=uploadsPerFrame;
    Reset();
}
void HlodProxyCache::Reset()
{
    lastUsed_.clear();bytes_.clear();present_.clear();touched_.clear();
    uploadsThisFrame_=0;frame_=0;stats_={};
}
void HlodProxyCache::Ensure(std::uint32_t cell) const
{
    if(cell<lastUsed_.size())return;
    const auto size=static_cast<std::size_t>(cell)+1;
    lastUsed_.resize(size,0);bytes_.resize(size,0);present_.resize(size,0);touched_.resize(size,0);
}
void HlodProxyCache::BeginFrame()
{
    ++frame_;uploadsThisFrame_=0;++stats_.frames;
    std::fill(touched_.begin(),touched_.end(),0);
}
bool HlodProxyCache::CanUpload() const
{
    return uploadsPerFrame_==0||uploadsThisFrame_<uploadsPerFrame_;
}
bool HlodProxyCache::Fits(std::uint64_t bytes) const
{
    if(budget_==0)return true;
    return bytes<=budget_&&stats_.bytes<=budget_-bytes;
}
void HlodProxyCache::AddUpload(std::uint64_t bytes)
{
    ++uploadsThisFrame_;++stats_.uploads;stats_.uploadedBytes+=bytes;
}
void HlodProxyCache::Touch(std::uint32_t cell)
{
    Ensure(cell);lastUsed_[cell]=frame_;touched_[cell]=1;
}
void HlodProxyCache::Insert(std::uint32_t cell,std::uint64_t bytes)
{
    Ensure(cell);
    Erase(cell);
    present_[cell]=1;bytes_[cell]=bytes;lastUsed_[cell]=frame_;
    stats_.bytes+=bytes;stats_.peakBytes=std::max(stats_.peakBytes,stats_.bytes);
}
void HlodProxyCache::Erase(std::uint32_t cell)
{
    if(cell>=present_.size()||!present_[cell])return;
    present_[cell]=0;stats_.bytes-=bytes_[cell];bytes_[cell]=0;
}
bool HlodProxyCache::Present(std::uint32_t cell) const
{
    return cell<present_.size()&&present_[cell];
}
bool HlodProxyCache::OverBudget() const
{
    return budget_!=0&&stats_.bytes>budget_;
}
bool HlodProxyCache::SelectEviction(std::uint32_t& cell) const
{
    if(budget_==0)return false;
    std::uint64_t oldest=std::numeric_limits<std::uint64_t>::max();
    bool found=false;
    for(std::size_t index=0;index<present_.size();++index)
    {
        if(!present_[index]||touched_[index])continue;
        if(!found||lastUsed_[index]<oldest){oldest=lastUsed_[index];cell=static_cast<std::uint32_t>(index);found=true;}
    }
    return found;
}
}
