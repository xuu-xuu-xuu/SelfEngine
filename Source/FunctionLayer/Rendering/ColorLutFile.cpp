#include "ColorLutFile.h"
#include <system_error>
#include <utility>

namespace se
{
namespace
{
// 读取失败时把具体错误键放进 error；stat 失败使用 Assets.ReadFailed。
bool ReadAsset(const std::filesystem::path& path,ColorLutAsset& output,std::string& error)
{
    return ReadColorLutAsset(path,output,error);
}
}
ColorLutFile::ColorLutFile(float pollIntervalSeconds) noexcept:pollInterval_(pollIntervalSeconds>0?pollIntervalSeconds:0.0f)
{
}
bool ColorLutFile::Open(const std::filesystem::path& path,std::string& error)
{
    path_=path;
    lut_.Clear();
    values_.reset();
    status_={};
    haveStamp_=false;
    lastPoll_=Clock::now();
    if(path_.empty()){error.clear();return false;}
    Reload(true);
    error=status_.error;
    return status_.loaded;
}
void ColorLutFile::Close() noexcept
{
    path_.clear();
    lut_.Clear();
    values_.reset();
    status_={};
    haveStamp_=false;
    lastPoll_=Clock::now();
}
void ColorLutFile::SetPollInterval(float seconds) noexcept
{
    pollInterval_=seconds>0?seconds:0.0f;
}
bool ColorLutFile::Poll(bool force)
{
    if(path_.empty())return false;
    const auto now=Clock::now();
    if(!force&&now-lastPoll_<std::chrono::duration<float>(pollInterval_))return false;
    lastPoll_=now;
    return Reload(force);
}
bool ColorLutFile::Reload(bool force)
{
    std::error_code error;
    const auto stampTime=std::filesystem::last_write_time(path_,error);
    if(error)
    {
        // 文件暂时不可见（编辑器保存时的原子替换、目录被移走）时保留旧 LUT，
        // 只在首次发现缺失或错误键变化时记录，避免每帧刷同一条日志。
        if(status_.error!="Assets.ReadFailed")
        {
            status_.error="Assets.ReadFailed";
            status_.detail=error.message();
        }
        haveStamp_=false;
        return false;
    }
    const auto stampSize=std::filesystem::file_size(path_,error);
    if(error)
    {
        status_.error="Assets.ReadFailed";status_.detail=error.message();haveStamp_=false;return false;
    }
    if(!force&&haveStamp_&&stampTime==stampTime_&&stampSize==stampSize_)return false;
    stampTime_=stampTime;
    stampSize_=stampSize;
    haveStamp_=true;
    ColorLutAsset asset;
    std::string errorKey;
    if(!ReadAsset(path_,asset,errorKey)||!asset.Valid())
    {
        ++status_.failures;
        status_.error=errorKey.empty()?"Rendering.LutAssetInvalid":errorKey;
        status_.detail=path_.filename().string();
        return false;
    }
    if(asset.colorSpace==ColorLutColorSpace::Srgb)
        for(auto& value:asset.values)value=SrgbToLinear(value);
    ColorLut next;
    if(!next.Adopt(asset.size,std::move(asset.values),asset.colorSpace))
    {
        ++status_.failures;
        status_.error="Rendering.LutAssetInvalid";
        status_.detail=path_.filename().string();
        return false;
    }
    lut_=std::move(next);
    values_=std::shared_ptr<const std::vector<Vec3>>(new std::vector<Vec3>(lut_.Values()));
    ++status_.revision;
    status_.loaded=true;
    status_.size=asset.size;
    status_.colorSpace=asset.colorSpace;
    status_.error.clear();
    status_.detail.clear();
    if(status_.revision>1)++status_.reloads;
    return true;
}
}
