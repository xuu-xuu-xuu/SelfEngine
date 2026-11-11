#include "Primitives.h"
#include "CoreLayer/Math/Math.h"
#include <stdexcept>

namespace se
{
MeshData MakeUnitCube()
{
    MeshData mesh;
    mesh.name="Cube";
    mesh.vertices.reserve(24);mesh.indices.reserve(36);
    const auto face=[&](Vec3 normal,Vec3 u,Vec3 v) {
        const auto base=static_cast<std::uint32_t>(mesh.vertices.size());
        const Vec3 center=normal*0.5f;
        const Vec3 positions[]{center-(u+v)*0.5f,center+(u-v)*0.5f,center+(u+v)*0.5f,center+(-u+v)*0.5f};
        const std::array<float,2> coordinates[]{ {0,1},{1,1},{1,0},{0,0} };
        for(std::size_t index=0;index<4;++index)
            mesh.vertices.push_back({{positions[index].x,positions[index].y,positions[index].z},
                {normal.x,normal.y,normal.z},coordinates[index],{u.x,u.y,u.z,-1}});
        for(const auto index: {0u,1u,2u,0u,2u,3u})mesh.indices.push_back(base+index);
    };
    face({0,0,1},{1,0,0},{0,1,0});face({0,0,-1},{-1,0,0},{0,1,0});
    face({1,0,0},{0,0,-1},{0,1,0});face({-1,0,0},{0,0,1},{0,1,0});
    face({0,1,0},{1,0,0},{0,0,-1});face({0,-1,0},{1,0,0},{0,0,1});
    return mesh;
}
MeshData MakeUnitSphere(std::uint32_t segments,std::uint32_t rings)
{
    if(segments<3||segments>512||rings<2||rings>256)throw std::invalid_argument("Assets.SphereSubdivisionsInvalid");
    MeshData mesh;mesh.name="Sphere";mesh.vertices.reserve(size_t(segments+1)*(rings+1));
    mesh.indices.reserve(size_t(segments)*(rings-1)*6);
    for(std::uint32_t row=0;row<=rings;++row)
    {
        const float v=float(row)/float(rings),theta=Pi*v;
        const float s=row==0||row==rings?0.0f:std::sin(theta),c=row==0?1.0f:row==rings?-1.0f:std::cos(theta);
        for(std::uint32_t column=0;column<=segments;++column)
        {
            const float u=float(column)/float(segments),phi=column==segments?0.0f:2*Pi*u,cp=std::cos(phi),sp=std::sin(phi);
            const std::array<float,3> normal{s*cp,c,s*sp};
            mesh.vertices.push_back({{normal[0]*0.5f,normal[1]*0.5f,normal[2]*0.5f},normal,{u,v},{-sp,0,cp,1}});
        }
    }
    for(std::uint32_t row=0;row<rings;++row)for(std::uint32_t column=0;column<segments;++column)
    {
        const auto a=row*(segments+1)+column,b=a+1,d=a+segments+1,c=d+1;
        if(row>0)mesh.indices.insert(mesh.indices.end(),{a,b,c});
        if(row+1<rings)mesh.indices.insert(mesh.indices.end(),{a,c,d});
    }
    return mesh;
}
}
