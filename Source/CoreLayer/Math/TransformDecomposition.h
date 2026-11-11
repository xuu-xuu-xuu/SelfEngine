#pragma once
#include "Math.h"
#include <algorithm>
#include <limits>

namespace se
{
// 只接受可重建的非奇异 TRS；镜像统一放在 X 尺度，不以近似分解丢掉剪切。
inline bool TryDecomposeTrs(const Mat4& matrix,Transform& output,float tolerance=1e-4f)
{
    if(!IsFinite(tolerance)||tolerance<=0||tolerance>.01f)return false;
    for(const auto value:matrix.values)if(!IsFinite(value))return false;
    if(std::abs(matrix(3,0))>tolerance||std::abs(matrix(3,1))>tolerance||std::abs(matrix(3,2))>tolerance||std::abs(matrix(3,3)-1)>tolerance)return false;
    Vec3 columns[3]={{matrix(0,0),matrix(1,0),matrix(2,0)},{matrix(0,1),matrix(1,1),matrix(2,1)},{matrix(0,2),matrix(1,2),matrix(2,2)}};
    float scales[3]{};
    for(int index=0;index<3;++index)
    {
        const auto length=std::hypot(double(columns[index].x),double(columns[index].y),double(columns[index].z));
        if(!std::isfinite(length)||length<=1e-8||length>(std::numeric_limits<float>::max)())return false;
        scales[index]=static_cast<float>(length);columns[index]=columns[index]/scales[index];
    }
    if(std::abs(Dot(columns[0],columns[1]))>tolerance||std::abs(Dot(columns[0],columns[2]))>tolerance||std::abs(Dot(columns[1],columns[2]))>tolerance)return false;
    const auto determinant=Dot(columns[0],Cross(columns[1],columns[2]));
    if(!IsFinite(determinant)||std::abs(std::abs(determinant)-1)>4*tolerance)return false;
    if(determinant<0){scales[0]=-scales[0];columns[0]=-columns[0];}
    const auto& x=columns[0];const auto& y=columns[1];const auto& z=columns[2];
    const float trace=x.x+y.y+z.z;Quat rotation;
    if(trace>0)
    {
        const float s=2*std::sqrt(trace+1);if(s<=1e-7f)return false;
        rotation={(y.z-z.y)/s,(z.x-x.z)/s,(x.y-y.x)/s,s*.25f};
    }
    else if(x.x>y.y&&x.x>z.z)
    {
        const float s=2*std::sqrt(std::max(0.0f,1+x.x-y.y-z.z));if(s<=1e-7f)return false;
        rotation={s*.25f,(y.x+x.y)/s,(z.x+x.z)/s,(y.z-z.y)/s};
    }
    else if(y.y>z.z)
    {
        const float s=2*std::sqrt(std::max(0.0f,1+y.y-x.x-z.z));if(s<=1e-7f)return false;
        rotation={(y.x+x.y)/s,s*.25f,(z.y+y.z)/s,(z.x-x.z)/s};
    }
    else
    {
        const float s=2*std::sqrt(std::max(0.0f,1+z.z-x.x-y.y));if(s<=1e-7f)return false;
        rotation={(z.x+x.z)/s,(z.y+y.z)/s,s*.25f,(x.y-y.x)/s};
    }
    Transform candidate{{matrix(0,3),matrix(1,3),matrix(2,3)},Normalize(rotation),{scales[0],scales[1],scales[2]}};
    if(!IsValid(candidate))return false;
    const auto rebuilt=candidate.Matrix();
    for(size_t index=0;index<16;++index)
        if(!IsFinite(rebuilt.values[index])||std::abs(rebuilt.values[index]-matrix.values[index])>tolerance*std::max(1.0f,std::abs(matrix.values[index])))return false;
    output=candidate;return true;
}
}
