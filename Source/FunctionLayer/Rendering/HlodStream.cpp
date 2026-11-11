#include "HlodStream.h"
#include <algorithm>
#include <cmath>

namespace se
{
bool HlodStream::Load(const std::filesystem::path& path,std::string& error)
{
    HlodScene loaded;
    if(!ReadHlodScene(path,loaded,error))return false;
    scene_=std::move(loaded);
    resident_.assign(scene_.cells.size(),true);
    loaded_=true;
    return true;
}
void HlodStream::Adopt(HlodScene&& scene)
{
    scene_=std::move(scene);
    resident_.assign(scene_.cells.size(),true);
    loaded_=true;
}
std::size_t HlodStream::UpdateResidency(const Vec3& camera,float radiusMeters)
{
    if(!loaded_)
    {
        resident_.clear();
        return 0;
    }
    if(!(radiusMeters>0)||!std::isfinite(radiusMeters))
    {
        resident_.assign(scene_.cells.size(),true);
        return resident_.size();
    }
    resident_.assign(scene_.cells.size(),false);
    std::size_t count=0;
    for(std::size_t index=0;index<scene_.cells.size();++index)
    {
        const auto& cell=scene_.cells[index];
        const float dx=cell.center.x-camera.x,dz=cell.center.z-camera.z;
        const auto distance=std::sqrt(dx*dx+dz*dz);
        if(distance<=radiusMeters+cell.radius)
        {
            resident_[index]=true;
            ++count;
        }
    }
    return count;
}
std::size_t HlodStream::ResidentCount() const noexcept
{
    return static_cast<std::size_t>(std::count(resident_.begin(),resident_.end(),true));
}
void HlodStream::Clear() noexcept
{
    scene_=HlodScene{};
    resident_.clear();
    loaded_=false;
}
}
