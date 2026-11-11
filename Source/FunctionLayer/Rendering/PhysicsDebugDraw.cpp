#include "PhysicsDebugDraw.h"
#include <cmath>

namespace se
{
bool BuildDebugSegmentTransform(const DebugLine& line,float width,Mat4& output)
{
    if(!IsFinite(line.from)||!IsFinite(line.to)||!IsFinite(line.color)||!IsFinite(width)||width<.001f||width>1
        ||line.color.x<0||line.color.y<0||line.color.z<0)return false;
    const auto delta=line.to-line.from;const double length=std::hypot(double(delta.x),double(delta.y),double(delta.z));
    if(!std::isfinite(length)||length<1e-6||length>1e6)return false;
    const auto direction=delta/static_cast<float>(length);const auto side=Normalize(Cross(direction,std::abs(direction.y)<.9f?Vec3{0,1,0}:Vec3{1,0,0}));
    const auto up=Normalize(Cross(side,direction));const auto center=line.from+delta*.5f;Mat4 candidate;
    const auto component=[](Vec3 value,unsigned row){return row==0?value.x:row==1?value.y:value.z;};
    for(unsigned row=0;row<3;++row){candidate(row,0)=component(delta,row);candidate(row,1)=component(up,row)*width;candidate(row,2)=component(side,row)*width;candidate(row,3)=component(center,row);}
    candidate(3,3)=1;if(!IsFinite(candidate))return false;output=candidate;return true;
}
rhi::Result AppendPhysicsDebugDraws(const StaticRenderModel& unitCube,std::span<const DebugLine> lines,
    rhi::PipelineHandle pipeline,const Mat4& viewProjection,std::vector<rhi::DrawIndexed>& output,float width)
{
    if(lines.size()>MaximumDebugSegments)return {rhi::Error::CapacityExceeded};
    if(unitCube.meshes.size()!=1||!unitCube.meshes[0].material)return {rhi::Error::InvalidArgument};
    std::vector<Mat4> transforms;transforms.reserve(lines.size());
    for(const auto& line:lines){Mat4 transform;if(!BuildDebugSegmentTransform(line,width,transform))return {rhi::Error::InvalidArgument};transforms.push_back(transform);}
    const auto& mesh=unitCube.meshes[0];const auto& material=*mesh.material;output.reserve(output.size()+lines.size());
    for(std::size_t index=0;index<lines.size();++index)
    {
        rhi::DrawIndexed draw;draw.vertices=*mesh.vertices;draw.indices=*mesh.indices;draw.pipeline=pipeline;draw.texture=*material.metallicRoughness;
        draw.normalTexture=*material.normal;draw.metallicRoughnessTexture=*material.metallicRoughness;draw.world=transforms[index];draw.viewProjection=viewProjection;
        const auto color=lines[index].color;draw.tint={color.x,color.y,color.z,1};draw.unlit=true;draw.castsShadow=false;draw.pbr.receivesShadow=false;output.push_back(draw);
    }
    return {};
}
}
