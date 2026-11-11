#pragma once
#include "StaticModelRender.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"

namespace se
{
inline constexpr std::size_t MaximumDebugSegments=4096;
bool BuildDebugSegmentTransform(const DebugLine& line,float width,Mat4& output);
rhi::Result AppendPhysicsDebugDraws(const StaticRenderModel& unitCube,std::span<const DebugLine> lines,
    rhi::PipelineHandle pipeline,const Mat4& viewProjection,std::vector<rhi::DrawIndexed>& output,float width=.03f);
}
