#include "LodBake.h"
#include "AssetIO.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace se
{
namespace
{
constexpr std::uint32_t LodFileVersion=1;
constexpr std::uint32_t LodMaximumLevels=8;
constexpr char LodMagic[4]={'S','E','L','D'};

struct Bounds
{
    std::array<float,3> minimum{},maximum{};
    bool valid=false;
};
Bounds ComputeBounds(const std::vector<MeshVertex>& vertices)
{
    Bounds bounds;
    if(vertices.empty())return bounds;
    bounds.minimum=vertices[0].position;bounds.maximum=vertices[0].position;
    for(const auto& vertex:vertices)for(int axis=0;axis<3;++axis)
    {
        bounds.minimum[axis]=(std::min)(bounds.minimum[axis],vertex.position[axis]);
        bounds.maximum[axis]=(std::max)(bounds.maximum[axis],vertex.position[axis]);
    }
    bounds.valid=true;return bounds;
}
std::uint64_t EdgeKey(std::uint32_t a,std::uint32_t b)
{
    if(a>b)std::swap(a,b);
    return (std::uint64_t(a)<<32)|b;
}
// 只被一个三角形使用的边就是开放边界；两端顶点锁定，简化不移动它们。
std::vector<bool> LockedBoundary(const std::vector<std::uint32_t>& indices,std::size_t vertexCount)
{
    std::map<std::uint64_t,int> edges;
    for(std::size_t triangle=0;triangle+2<indices.size();triangle+=3)
        for(int edge=0;edge<3;++edge)
        {
            const auto a=indices[triangle+edge],b=indices[triangle+(edge+1)%3];
            ++edges[EdgeKey(a,b)];
        }
    std::vector<bool> locked(vertexCount,false);
    for(const auto& [key,count]:edges)if(count==1)
    {
        locked[static_cast<std::size_t>(key>>32)]=true;
        locked[static_cast<std::size_t>(key&0xFFFFFFFFu)]=true;
    }
    return locked;
}
// 顶点聚类：未锁定顶点按 cell 量化到代表点（取首次出现的顶点），锁定顶点保持自身。
// 返回新索引，并把被合并顶点的最大位移写进 worldError。
void ClusterLevel(const std::vector<MeshVertex>& vertices,const std::vector<std::uint32_t>& source,
    const std::vector<bool>& locked,float cellSize,std::vector<std::uint32_t>& output,float& worldError)
{
    std::map<std::tuple<int,int,int>,std::uint32_t> cells;
    std::vector<std::uint32_t> remap(vertices.size(),UINT32_MAX);
    const auto quantize=[cellSize](float value)
    {
        return static_cast<int>(std::floor(value/cellSize));
    };
    for(std::uint32_t index=0;index<vertices.size();++index)
    {
        if(locked[index]||index>=remap.size())
        {
            remap[index]=index;
            continue;
        }
        const auto& position=vertices[index].position;
        const auto key=std::make_tuple(quantize(position[0]),quantize(position[1]),quantize(position[2]));
        const auto found=cells.find(key);
        if(found==cells.end())
        {
            cells.emplace(key,index);
            remap[index]=index;
            continue;
        }
        remap[index]=found->second;
        const auto& representative=vertices[found->second].position;
        const float dx=position[0]-representative[0],dy=position[1]-representative[1],dz=position[2]-representative[2];
        worldError=(std::max)(worldError,std::sqrt(dx*dx+dy*dy+dz*dz));
    }
    output.clear();output.reserve(source.size());
    for(std::size_t triangle=0;triangle+2<source.size();triangle+=3)
    {
        const auto a=remap[source[triangle]],b=remap[source[triangle+1]],c=remap[source[triangle+2]];
        if(a==UINT32_MAX||b==UINT32_MAX||c==UINT32_MAX)continue;
        if(a==b||b==c||a==c)continue;
        output.push_back(a);output.push_back(b);output.push_back(c);
    }
}
void AppendUint32(std::vector<std::uint8_t>& bytes,std::uint32_t value)
{
    for(int shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>((value>>shift)&0xFF));
}
void AppendFloat(std::vector<std::uint8_t>& bytes,float value)
{
    std::uint32_t raw=0;std::memcpy(&raw,&value,sizeof(raw));AppendUint32(bytes,raw);
}
std::uint32_t ReadUint32(const std::uint8_t* bytes)
{
    std::uint32_t value=0;
    for(int shift=0;shift<32;shift+=8)value|=static_cast<std::uint32_t>(*bytes++)<<shift;
    return value;
}
float ReadFloat(const std::uint8_t* bytes)
{
    const auto raw=ReadUint32(bytes);float value=0;std::memcpy(&value,&raw,sizeof(value));return value;
}
}
bool BakeLodChain(const StaticModel& model,std::uint32_t mesh,std::uint32_t levels,
    LodChain& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(mesh>=model.meshes.size()||levels<2||levels>LodMaximumLevels)
    {
        asset_io::Diagnostic(diagnostics,"Rendering.LodAssetInvalid");
        return false;
    }
    const auto& source=model.meshes[mesh];
    const auto& vertices=source.vertices;
    if(vertices.empty()||source.indices.empty()||source.indices.size()%3!=0)return false;
    for(const auto index:source.indices)if(index>=vertices.size())return false;
    const auto bounds=ComputeBounds(vertices);
    if(!bounds.valid)return false;
    const float extent=(std::max)({bounds.maximum[0]-bounds.minimum[0],bounds.maximum[1]-bounds.minimum[1],
        bounds.maximum[2]-bounds.minimum[2]});
    if(!(extent>0))return false;
    const auto center=std::array<float,3>{(bounds.minimum[0]+bounds.maximum[0])*0.5f,
        (bounds.minimum[1]+bounds.maximum[1])*0.5f,(bounds.minimum[2]+bounds.maximum[2])*0.5f};
    float radius=0;
    for(const auto& vertex:vertices)
    {
        const float dx=vertex.position[0]-center[0],dy=vertex.position[1]-center[1],dz=vertex.position[2]-center[2];
        radius=(std::max)(radius,std::sqrt(dx*dx+dy*dy+dz*dz));
    }
    const auto locked=LockedBoundary(source.indices,vertices.size());
    LodChain candidate;candidate.mesh=mesh;
    candidate.vertexCount=static_cast<std::uint32_t>(vertices.size());
    candidate.radius=radius;
    for(const auto value:locked)candidate.lockedVertices+=value;
    candidate.levels.push_back({source.indices,0});
    for(std::uint32_t level=1;level<levels;++level)
    {
        LodLevel simplified;
        // 每级 cell 约为包围盒最长边的 2^level/16，保证级别越粗合并越多。
        const float cellSize=extent*static_cast<float>(1u<<level)/16.0f;
        ClusterLevel(vertices,source.indices,locked,cellSize,simplified.indices,simplified.worldError);
        if(simplified.indices.empty())return false;
        candidate.levels.push_back(std::move(simplified));
    }
    // 级别必须单调变粗，否则无法用误差做选择。
    for(std::size_t level=1;level<candidate.levels.size();++level)
        if(candidate.levels[level].indices.size()>=candidate.levels[level-1].indices.size())
        {
            asset_io::Diagnostic(diagnostics,"Rendering.LodAssetInvalid");
            return false;
        }
    for(std::size_t level=1;level<candidate.levels.size();++level)
        candidate.levels[level].worldError=(std::max)(candidate.levels[level].worldError,
            candidate.levels[level-1].worldError);
    output=std::move(candidate);
    return true;
}
bool WriteLodChain(const std::filesystem::path& path,const LodChain& chain,std::string& error)
{
    if(chain.levels.size()<2||chain.levels.size()>LodMaximumLevels||!chain.vertexCount
        ||!(chain.radius>0)||!std::isfinite(chain.radius)){error="Rendering.LodAssetInvalid";return false;}
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(),LodMagic,LodMagic+4);
    AppendUint32(bytes,LodFileVersion);AppendUint32(bytes,chain.mesh);
    AppendUint32(bytes,chain.vertexCount);AppendFloat(bytes,chain.radius);
    AppendUint32(bytes,static_cast<std::uint32_t>(chain.levels.size()));
    for(const auto& level:chain.levels)
    {
        if(level.indices.size()%3||level.indices.size()>0xFFFFFFFFull/4)
        {error="Rendering.LodAssetInvalid";return false;}
        AppendUint32(bytes,static_cast<std::uint32_t>(level.indices.size()));
        AppendFloat(bytes,level.worldError);
        for(const auto index:level.indices)
        {
            if(index>=chain.vertexCount){error="Rendering.LodAssetInvalid";return false;}
            AppendUint32(bytes,index);
        }
    }
    if(!asset_io::WriteAtomic(path,bytes,true)){error="Assets.WriteFailed";return false;}
    return true;
}
bool ReadLodChain(const std::filesystem::path& path,LodChain& output,std::string& error)
{
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)||bytes.size()<4+4*4){error="Assets.ReadFailed";return false;}
    if(std::memcmp(bytes.data(),LodMagic,4)!=0)return false;
    const auto* cursor=bytes.data()+4;
    const auto version=ReadUint32(cursor);cursor+=4;
    LodChain candidate;
    candidate.mesh=ReadUint32(cursor);cursor+=4;
    candidate.vertexCount=ReadUint32(cursor);cursor+=4;
    candidate.radius=ReadFloat(cursor);cursor+=4;
    const auto levelCount=ReadUint32(cursor);cursor+=4;
    if(version!=LodFileVersion||!candidate.vertexCount||levelCount<2||levelCount>LodMaximumLevels
        ||!(candidate.radius>0)||!std::isfinite(candidate.radius))return false;
    for(std::uint32_t level=0;level<levelCount;++level)
    {
        if(cursor+8>bytes.data()+bytes.size())return false;
        const auto indexCount=ReadUint32(cursor);cursor+=4;
        const auto worldError=ReadFloat(cursor);cursor+=4;
        if(indexCount%3||indexCount>0xFFFFFFFFull/4)return false;
        if(cursor+std::size_t(indexCount)*4>bytes.data()+bytes.size())return false;
        LodLevel entry;entry.indices.resize(indexCount);entry.worldError=worldError;
        for(std::uint32_t index=0;index<indexCount;++index){entry.indices[index]=ReadUint32(cursor);cursor+=4;}
        for(const auto index:entry.indices)if(index>=candidate.vertexCount)return false;
        candidate.levels.push_back(std::move(entry));
    }
    if(cursor!=bytes.data()+bytes.size())return false;
    for(std::size_t level=1;level<candidate.levels.size();++level)
        if(candidate.levels[level].indices.size()>=candidate.levels[level-1].indices.size())return false;
    output=std::move(candidate);return true;
}
}
