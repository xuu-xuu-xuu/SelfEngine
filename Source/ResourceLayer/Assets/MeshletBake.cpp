#include "MeshletBake.h"
#include "AssetIO.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace se
{
namespace
{
constexpr std::uint32_t MeshletFileVersion=1;
constexpr std::uint32_t MeshletMaximumClusters=1u<<20;
constexpr char MeshletMagic[4]={'S','E','M','S'};
void AppendUint32(std::vector<std::uint8_t>& bytes,std::uint32_t value)
{
    for(int shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>((value>>shift)&0xFF));
}
void AppendFloat(std::vector<std::uint8_t>& bytes,float value)
{
    std::uint32_t raw=0;std::memcpy(&raw,&value,4);AppendUint32(bytes,raw);
}
std::uint32_t ReadUint32(const std::uint8_t*& bytes,const std::uint8_t* end)
{
    if(end-bytes<4){bytes=end;return 0;}
    std::uint32_t value=0;for(int shift=0;shift<32;shift+=8)value|=static_cast<std::uint32_t>(*bytes++)<<shift;return value;
}
float ReadFloat(const std::uint8_t*& bytes,const std::uint8_t* end)
{
    const auto raw=ReadUint32(bytes,end);float value=0;std::memcpy(&value,&raw,4);return value;
}
void Diagnostic(std::vector<AssetDiagnostic>& diagnostics,const char* key,const char* detail)
{
    diagnostics.push_back({key,detail,false});
}
Vec3 Position(const MeshVertex& vertex){return {vertex.position[0],vertex.position[1],vertex.position[2]};}
Vec3 Normal(const MeshVertex& vertex){return {vertex.normal[0],vertex.normal[1],vertex.normal[2]};}
bool Finite(Vec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
void ComputeBounds(const MeshData& mesh,MeshletCluster& cluster)
{
    Vec3 minimum{1e30f,1e30f,1e30f},maximum{-1e30f,-1e30f,-1e30f};
    for(const auto index:cluster.vertices)
    {
        const auto position=Position(mesh.vertices[index]);
        minimum={std::min(minimum.x,position.x),std::min(minimum.y,position.y),std::min(minimum.z,position.z)};
        maximum={std::max(maximum.x,position.x),std::max(maximum.y,position.y),std::max(maximum.z,position.z)};
    }
    cluster.center={(minimum.x+maximum.x)*0.5f,(minimum.y+maximum.y)*0.5f,(minimum.z+maximum.z)*0.5f};
    cluster.radius=0;
    for(const auto index:cluster.vertices)cluster.radius=std::max(cluster.radius,Length(Position(mesh.vertices[index])-cluster.center));
    Vec3 sum{0,0,0};float maximumAngle=0;std::uint32_t normals=0;
    for(std::size_t index=0;index+2<cluster.indices.size();index+=3)
    {
        const auto a=Position(mesh.vertices[cluster.vertices[cluster.indices[index]]]);
        const auto b=Position(mesh.vertices[cluster.vertices[cluster.indices[index+1]]]);
        const auto c=Position(mesh.vertices[cluster.vertices[cluster.indices[index+2]]]);
        const auto cross=Cross(b-a,c-a);const auto area=Length(cross);
        if(!(area>1e-12f))continue;
        const auto normal=cross/area;sum=sum+normal;++normals;
    }
    cluster.coneValid=false;cluster.coneAxis={0,0,1};cluster.coneCutoff=0;
    const auto axisLength=Length(sum);
    if(normals==0||!(axisLength>1e-8f))return;
    cluster.coneAxis=sum/axisLength;
    for(std::size_t index=0;index+2<cluster.indices.size();index+=3)
    {
        const auto a=Position(mesh.vertices[cluster.vertices[cluster.indices[index]]]);
        const auto b=Position(mesh.vertices[cluster.vertices[cluster.indices[index+1]]]);
        const auto c=Position(mesh.vertices[cluster.vertices[cluster.indices[index+2]]]);
        const auto cross=Cross(b-a,c-a);const auto area=Length(cross);
        if(!(area>1e-12f))continue;
        const auto cosine=std::clamp(Dot(cross/area,cluster.coneAxis),-1.0f,1.0f);
        maximumAngle=std::max(maximumAngle,std::acos(cosine));
    }
    cluster.coneCutoff=maximumAngle;
    // 半角超过 90° 时锥体几乎覆盖整个球面，测试不再保守；只保留视锥剔除。
    cluster.coneValid=maximumAngle<=Pi*0.5f;
}
bool ValidView(const MeshletView& view)
{
    if(!Finite(view.position))return false;
    for(const auto& plane:view.frustum.planes)
    {
        if(!std::isfinite(plane.x)||!std::isfinite(plane.y)||!std::isfinite(plane.z)||!std::isfinite(plane.d))return false;
        const double squared=double(plane.x)*plane.x+double(plane.y)*plane.y+double(plane.z)*plane.z;
        if(squared<1e-40)return false;
    }
    return true;
}
}
bool BakeMeshlets(const MeshData& mesh,MeshletSet& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(mesh.vertices.empty()||mesh.indices.empty()||mesh.indices.size()%3!=0)
    {Diagnostic(diagnostics,"Assets.MeshletInvalid","mesh");return false;}
    for(const auto index:mesh.indices)if(index>=mesh.vertices.size())
    {Diagnostic(diagnostics,"Assets.MeshletInvalid","index");return false;}
    MeshletSet next;next.sourceVertexCount=static_cast<std::uint32_t>(mesh.vertices.size());
    next.triangleCount=static_cast<std::uint32_t>(mesh.indices.size()/3);
    std::vector<MeshletCluster> clusters;
    const auto triangleCount=next.triangleCount;
    // 按边邻接生长，保证 cluster 是空间局部面片，法线锥才有意义；每个三角形只属于一个 cluster。
    std::unordered_map<std::uint64_t,std::vector<std::uint32_t>> edges;
    const auto edgeKey=[](std::uint32_t a,std::uint32_t b)
    {
        if(a>b)std::swap(a,b);
        return (static_cast<std::uint64_t>(a)<<32)|b;
    };
    for(std::uint32_t triangle=0;triangle<triangleCount;++triangle)
    {
        const auto a=mesh.indices[triangle*3],b=mesh.indices[triangle*3+1],c=mesh.indices[triangle*3+2];
        edges[edgeKey(a,b)].push_back(triangle);edges[edgeKey(b,c)].push_back(triangle);edges[edgeKey(c,a)].push_back(triangle);
    }
    std::vector<char> assigned(triangleCount,0);
    for(std::uint32_t seed=0;seed<triangleCount;++seed)
    {
        if(assigned[seed])continue;
        MeshletCluster current;std::unordered_map<std::uint32_t,std::uint32_t> remap;
        std::vector<std::uint32_t> frontier{seed};assigned[seed]=1;
        const auto append=[&](std::uint32_t triangle)
        {
            const std::uint32_t source[3]={mesh.indices[triangle*3],mesh.indices[triangle*3+1],mesh.indices[triangle*3+2]};
            for(int corner=0;corner<3;++corner)
            {
                const auto found=remap.find(source[corner]);
                if(found!=remap.end()){current.indices.push_back(found->second);continue;}
                const auto local=static_cast<std::uint32_t>(current.vertices.size());
                current.vertices.push_back(source[corner]);remap.emplace(source[corner],local);
                current.indices.push_back(local);
            }
        };
        append(seed);
        for(std::size_t cursor=0;cursor<frontier.size();++cursor)
        {
            const auto triangle=frontier[cursor];
            const std::uint32_t source[3]={mesh.indices[triangle*3],mesh.indices[triangle*3+1],mesh.indices[triangle*3+2]};
            for(int edge=0;edge<3;++edge)
            {
                const auto found=edges.find(edgeKey(source[edge],source[(edge+1)%3]));
                if(found==edges.end())continue;
                for(const auto neighbor:found->second)
                {
                    if(assigned[neighbor])continue;
                    if(current.indices.size()/3>=MeshletMaxTriangles)break;
                    const std::uint32_t candidate[3]={mesh.indices[neighbor*3],mesh.indices[neighbor*3+1],mesh.indices[neighbor*3+2]};
                    std::uint32_t newVertices=0;
                    for(int corner=0;corner<3;++corner)if(!remap.contains(candidate[corner]))++newVertices;
                    if(remap.size()+newVertices>MeshletMaxVertices)continue;
                    assigned[neighbor]=1;append(neighbor);frontier.push_back(neighbor);
                }
            }
        }
        ComputeBounds(mesh,current);
        clusters.push_back(std::move(current));
    }
    if(clusters.empty()){Diagnostic(diagnostics,"Assets.MeshletInvalid","clusters");return false;}
    next.clusters=std::move(clusters);output=std::move(next);return true;
}
bool WriteMeshletSet(const std::filesystem::path& path,const MeshletSet& set,std::string& error)
{
    if(set.sourceVertexCount==0||set.clusters.empty()||set.clusters.size()>MeshletMaximumClusters)
    {error="Meshlet.InvalidSet";return false;}
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(),MeshletMagic,MeshletMagic+4);
    AppendUint32(bytes,MeshletFileVersion);AppendUint32(bytes,set.sourceVertexCount);
    AppendUint32(bytes,set.triangleCount);AppendUint32(bytes,static_cast<std::uint32_t>(set.clusters.size()));
    for(const auto& cluster:set.clusters)
    {
        if(cluster.vertices.empty()||cluster.vertices.size()>MeshletMaxVertices
            ||cluster.indices.empty()||cluster.indices.size()%3||cluster.indices.size()/3>MeshletMaxTriangles
            ||!std::isfinite(cluster.radius)||cluster.radius<0||!Finite(cluster.center)||!Finite(cluster.coneAxis))
        {error="Meshlet.InvalidCluster";return false;}
        AppendUint32(bytes,static_cast<std::uint32_t>(cluster.vertices.size()));
        AppendUint32(bytes,static_cast<std::uint32_t>(cluster.indices.size()));
        for(int axis=0;axis<3;++axis)AppendFloat(bytes,axis==0?cluster.center.x:axis==1?cluster.center.y:cluster.center.z);
        AppendFloat(bytes,cluster.radius);
        for(int axis=0;axis<3;++axis)AppendFloat(bytes,axis==0?cluster.coneAxis.x:axis==1?cluster.coneAxis.y:cluster.coneAxis.z);
        AppendFloat(bytes,cluster.coneCutoff);AppendUint32(bytes,cluster.coneValid?1u:0u);
        for(const auto vertex:cluster.vertices)AppendUint32(bytes,vertex);
        for(const auto index:cluster.indices)AppendUint32(bytes,index);
    }
    if(!asset_io::WriteAtomic(path,bytes,true)){error="Assets.WriteFailed";return false;}
    return true;
}
bool ReadMeshletSet(const std::filesystem::path& path,MeshletSet& set,std::string& error)
{
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)||bytes.size()<20){error="Meshlet.ReadFailed";return false;}
    const auto* cursor=bytes.data();const auto* end=bytes.data()+bytes.size();
    if(std::memcmp(cursor,MeshletMagic,4)!=0){error="Meshlet.InvalidMagic";return false;}
    cursor+=4;
    if(ReadUint32(cursor,end)!=MeshletFileVersion){error="Meshlet.UnsupportedVersion";return false;}
    MeshletSet next;next.sourceVertexCount=ReadUint32(cursor,end);
    next.triangleCount=ReadUint32(cursor,end);
    const auto clusterCount=ReadUint32(cursor,end);
    if(next.sourceVertexCount==0||clusterCount==0||clusterCount>MeshletMaximumClusters)
    {error="Meshlet.InvalidSet";return false;}
    next.clusters.reserve(clusterCount);
    for(std::uint32_t cell=0;cell<clusterCount;++cell)
    {
        MeshletCluster cluster;
        const auto vertices=ReadUint32(cursor,end),indices=ReadUint32(cursor,end);
        if(vertices==0||vertices>MeshletMaxVertices||indices==0||indices%3||indices/3>MeshletMaxTriangles)
        {error="Meshlet.InvalidCluster";return false;}
        cluster.center={ReadFloat(cursor,end),ReadFloat(cursor,end),ReadFloat(cursor,end)};
        cluster.radius=ReadFloat(cursor,end);
        cluster.coneAxis={ReadFloat(cursor,end),ReadFloat(cursor,end),ReadFloat(cursor,end)};
        cluster.coneCutoff=ReadFloat(cursor,end);
        cluster.coneValid=ReadUint32(cursor,end)!=0;
        if(end-cursor<static_cast<std::ptrdiff_t>(vertices)*4){error="Meshlet.Truncated";return false;}
        cluster.vertices.reserve(vertices);
        for(std::uint32_t index=0;index<vertices;++index)
        {
            const auto vertex=ReadUint32(cursor,end);
            if(vertex>=next.sourceVertexCount){error="Meshlet.InvalidVertex";return false;}
            cluster.vertices.push_back(vertex);
        }
        if(end-cursor<static_cast<std::ptrdiff_t>(indices)*4){error="Meshlet.Truncated";return false;}
        cluster.indices.reserve(indices);
        for(std::uint32_t index=0;index<indices;++index)
        {
            const auto local=ReadUint32(cursor,end);
            if(local>=vertices){error="Meshlet.InvalidIndex";return false;}
            cluster.indices.push_back(local);
        }
        if(!std::isfinite(cluster.radius)||cluster.radius<0||!Finite(cluster.center)||!Finite(cluster.coneAxis))
        {error="Meshlet.InvalidCluster";return false;}
        next.clusters.push_back(std::move(cluster));
    }
    if(cursor!=end){error="Meshlet.TrailingBytes";return false;}
    set=std::move(next);return true;
}
void CullMeshlets(const MeshletSet& set,const MeshletView& view,MeshletCullResult& output)
{
    output={};
    if(!ValidView(view))
    {
        output.visible.resize(set.clusters.size());
        for(std::size_t index=0;index<set.clusters.size();++index)output.visible[index]=static_cast<std::uint32_t>(index);
        return;
    }
    output.visible.reserve(set.clusters.size());
    for(std::size_t index=0;index<set.clusters.size();++index)
    {
        const auto& cluster=set.clusters[index];
        bool visible=true;
        for(const auto& plane:view.frustum.planes)
        {
            const auto distance=plane.x*cluster.center.x+plane.y*cluster.center.y+plane.z*cluster.center.z+plane.d;
            if(distance<-cluster.radius){visible=false;break;}
        }
        if(visible&&view.coneCulling&&cluster.coneValid
            &&std::isfinite(cluster.coneCutoff)&&cluster.coneCutoff>=0&&cluster.coneCutoff<=Pi*0.5f)
        {
            const auto toCenter=cluster.center-view.position;const auto distance=Length(toCenter);
            // 距离远大于半径时视线方向在 cluster 内近似一致，锥测试才保守。
            if(distance>cluster.radius*8&&distance>1e-4f)
            {
                const auto cosine=Dot(toCenter/distance,cluster.coneAxis);
                if(cosine>std::sin(cluster.coneCutoff)){visible=false;++output.coneCulled;}
            }
        }
        if(visible)output.visible.push_back(static_cast<std::uint32_t>(index));else ++output.culled;
    }
}
}
