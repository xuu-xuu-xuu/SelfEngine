#pragma once

#include "CoreLayer/Math/Projection.h"
#include <limits>

namespace se
{
struct Aabb {Vec3 minimum{},maximum{};};
inline bool IsValid(const Aabb& box)
{
    return IsFinite(box.minimum)&&IsFinite(box.maximum)&&box.minimum.x<=box.maximum.x
        &&box.minimum.y<=box.maximum.y&&box.minimum.z<=box.maximum.z;
}
// 非仿射或奇异矩阵由调用方走保守保留路径，不能把错误变成不可见。
inline std::optional<Aabb> TransformBounds(const Aabb& box,const Mat4& matrix)
{
    if(!IsValid(box)||!IsFinite(matrix)||matrix(3,0)!=0||matrix(3,1)!=0||matrix(3,2)!=0||matrix(3,3)!=1||!Inverse(matrix))return std::nullopt;
    const double lo[]{box.minimum.x,box.minimum.y,box.minimum.z},hi[]{box.maximum.x,box.maximum.y,box.maximum.z};
    double center[3]{},extent[3]{};float low[3]{},high[3]{};
    for(size_t axis=0;axis<3;++axis){center[axis]=(lo[axis]+hi[axis])*0.5;extent[axis]=(hi[axis]-lo[axis])*0.5;}
    for(size_t row=0;row<3;++row)
    {
        double c=matrix(row,3),e=0;
        for(size_t column=0;column<3;++column){c+=matrix(row,column)*center[column];e+=std::abs(double(matrix(row,column)))*extent[column];}
        low[row]=std::nextafter(static_cast<float>(c-e),-std::numeric_limits<float>::infinity());
        high[row]=std::nextafter(static_cast<float>(c+e),std::numeric_limits<float>::infinity());
    }
    Aabb result{{low[0],low[1],low[2]},{high[0],high[1],high[2]}};
    return IsValid(result)?std::optional<Aabb>{result}:std::nullopt;
}
struct FrustumPlane {double x=0,y=0,z=0,d=0;};
struct Frustum {std::array<FrustumPlane,6> planes;};
inline std::optional<Frustum> BuildFrustum(const Mat4& clipFromWorld)
{
    if(!IsFinite(clipFromWorld))return std::nullopt;Frustum result;
    for(size_t index=0;index<6;++index)
    {
        double values[4]{};
        for(size_t column=0;column<4;++column)
            values[column]=index<4?double(clipFromWorld(3,column))+(index%2?-1.0:1.0)*clipFromWorld(index/2,column)
                :index==4?double(clipFromWorld(2,column)):double(clipFromWorld(3,column))-clipFromWorld(2,column);
        const double length=std::sqrt(values[0]*values[0]+values[1]*values[1]+values[2]*values[2]);
        if(!std::isfinite(length)||length<1e-20)return std::nullopt;
        result.planes[index]={values[0]/length,values[1]/length,values[2]/length,values[3]/length};
    }
    return result;
}
// 正顶点在负半空间外才拒绝；平面相交和数值异常均保留。
inline bool Intersects(const Frustum& frustum,const Aabb& box)
{
    if(!IsValid(box))return true;
    for(const auto& p:frustum.planes)
    {
        const double x=p.x*(p.x>=0?box.maximum.x:box.minimum.x),y=p.y*(p.y>=0?box.maximum.y:box.minimum.y),z=p.z*(p.z>=0?box.maximum.z:box.minimum.z);
        const double margin=1e-5+8*std::numeric_limits<float>::epsilon()*(std::abs(x)+std::abs(y)+std::abs(z)+std::abs(p.d));
        if(x+y+z+p.d < -margin)return false;
    }
    return true;
}
}
