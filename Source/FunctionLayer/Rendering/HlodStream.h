#pragma once
#include "ResourceLayer/Assets/HlodBake.h"
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
// G05.11：场景单元驻留。`.sehlod` 文件一次读入，按相机位置与半径计算驻留单元；
// 非驻留单元由调用方回退到实例绘制，驻留判定不改变几何，因此边界仍由单元划分保证。
class HlodStream
{
public:
    bool Load(const std::filesystem::path& path,std::string& error);
    // G05.13：接收后台读取完成的场景，避免第二次文件读取。
    void Adopt(HlodScene&& scene);
    bool Loaded() const noexcept {return loaded_;}
    const HlodScene& Scene() const noexcept {return scene_;}
    // radiusMeters<=0 表示全部驻留；返回本次驻留单元数。
    std::size_t UpdateResidency(const Vec3& camera,float radiusMeters);
    bool Resident(std::size_t cell) const noexcept {return cell<resident_.size()&&resident_[cell];}
    std::size_t ResidentCount() const noexcept;
    void Clear() noexcept;
private:
    HlodScene scene_;
    bool loaded_=false;
    std::vector<bool> resident_;
};
}
