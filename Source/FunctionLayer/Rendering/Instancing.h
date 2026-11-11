#pragma once
#include "RenderQueue.h"
#include "CoreLayer/Math/Bounds.h"

namespace se
{
struct PbrInstanceBatch
{
    rhi::DrawIndexed draw;
    std::vector<rhi::InstanceParameters> instances;
    // G06.10：与 instances 一一对应的世界包围盒；Build 未提供包围盒时为空。
    std::vector<Aabb> bounds;
};
class PbrInstanceBatcher
{
public:
    // bounds 与 draws 等长时把每个绘制的包围盒复制到对应实例，供多批次遮挡使用。
    std::span<const PbrInstanceBatch> Build(std::span<const rhi::DrawIndexed> draws,
        std::span<const Aabb> bounds = {});
    size_t InstanceCapacity() const;
    void Clear(){std::vector<PbrInstanceBatch>{}.swap(groups_);active_=0;}
private:
    std::vector<PbrInstanceBatch> groups_;
    size_t active_=0;
};
}
