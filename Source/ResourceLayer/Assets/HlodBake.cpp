#include "HlodBake.h"
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
constexpr std::uint32_t HlodFileVersion=1;
constexpr std::size_t HlodMaximumMaterialKey=256;
constexpr std::uint32_t HlodMaximumCells=1u<<20;
constexpr std::uint32_t HlodMaximumPartsPerCell=4096;
constexpr char HlodMagic[4]={'S','E','H','L'};

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
Vec3 ToVec3(const std::array<float,3>& value){return {value[0],value[1],value[2]};}
std::array<float,3> ToArray(Vec3 value){return {value.x,value.y,value.z};}
bool Finite(Vec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
void Diagnostic(std::vector<AssetDiagnostic>& diagnostics,const char* key,const char* detail)
{
    diagnostics.push_back({key,detail,false});
}
}

HlodCellCoordinate HlodCellFor(const Vec3& worldCenter,float cellSize)
{
    if(!(cellSize>0)||!std::isfinite(cellSize)||!Finite(worldCenter))return {};
    return {static_cast<std::int32_t>(std::floor(worldCenter.x/cellSize)),
        static_cast<std::int32_t>(std::floor(worldCenter.z/cellSize))};
}
void TransformMeshToWorld(const MeshData& source,const Mat4& world,MeshData& output)
{
    output=source;
    const auto inverse=Inverse(world);
    const auto normalFromWorld=inverse?Transpose(*inverse):Mat4{};
    for(auto& vertex:output.vertices)
    {
        const auto position=TransformPoint(world,ToVec3(vertex.position));
        vertex.position=ToArray(position);
        auto normal=TransformVector(normalFromWorld,ToVec3(vertex.normal));
        const auto normalLength=Length(normal);
        normal=normalLength>1e-8f?normal/normalLength:Vec3{0,1,0};
        vertex.normal=ToArray(normal);
        auto tangent=TransformVector(world,{vertex.tangent[0],vertex.tangent[1],vertex.tangent[2]});
        const auto tangentLength=Length(tangent);
        tangent=tangentLength>1e-8f?tangent/tangentLength:Vec3{1,0,0};
        vertex.tangent={tangent.x,tangent.y,tangent.z,vertex.tangent[3]};
    }
}
bool BakeHlod(const std::vector<HlodInstance>& instances,float cellSize,HlodScene& output,
    std::vector<AssetDiagnostic>& diagnostics,std::vector<HlodCellCoordinate>* assignments)
{
    if(assignments)assignments->clear();
    if(instances.empty()||!(cellSize>0)||!std::isfinite(cellSize))
    {
        Diagnostic(diagnostics,"Rendering.LodAssetInvalid","HLOD instances or cell size");
        return false;
    }
    std::map<std::tuple<std::int32_t,std::int32_t,std::string>,MeshData> parts;
    std::map<std::pair<std::int32_t,std::int32_t>,float> errors;
    std::uint32_t instanceCount=0;
    for(const auto& instance:instances)
    {
        if(!instance.mesh||instance.material.size()>HlodMaximumMaterialKey
            ||!std::isfinite(instance.worldError)||instance.worldError<0)
        {
            Diagnostic(diagnostics,"Rendering.LodAssetInvalid","HLOD instance");
            return false;
        }
        const auto& indices=instance.indices?*instance.indices:instance.mesh->indices;
        if(indices.empty()||indices.size()%3!=0)return false;
        for(const auto index:indices)if(index>=instance.mesh->vertices.size())return false;
        MeshData transformed;
        TransformMeshToWorld(*instance.mesh,instance.world,transformed);
        if(transformed.vertices.empty())return false;
        Vec3 minimum=ToVec3(transformed.vertices[0].position),maximum=minimum;
        for(const auto& vertex:transformed.vertices)
        {
            const auto position=ToVec3(vertex.position);
            minimum={ (std::min)(minimum.x,position.x),(std::min)(minimum.y,position.y),(std::min)(minimum.z,position.z) };
            maximum={ (std::max)(maximum.x,position.x),(std::max)(maximum.y,position.y),(std::max)(maximum.z,position.z) };
        }
        const auto coordinate=HlodCellFor((minimum+maximum)*0.5f,cellSize);
        if(assignments)assignments->push_back(coordinate);
        auto& part=parts[{coordinate.x,coordinate.z,instance.material}];
        if(part.name.empty())part.name=instance.material;
        const auto base=static_cast<std::uint32_t>(part.vertices.size());
        if(part.vertices.size()+transformed.vertices.size()>0xFFFFFFFFull)return false;
        part.vertices.insert(part.vertices.end(),transformed.vertices.begin(),transformed.vertices.end());
        for(const auto index:indices)part.indices.push_back(base+index);
        auto& error=errors[{coordinate.x,coordinate.z}];
        error=(std::max)(error,instance.worldError);
        ++instanceCount;
    }
    HlodScene candidate;candidate.cellSize=cellSize;candidate.instanceCount=instanceCount;
    std::pair<std::int32_t,std::int32_t> current{0,0};bool started=false;
    for(auto& [key,mesh]:parts)
    {
        const auto& [x,z,material]=key;
        const std::pair<std::int32_t,std::int32_t> coordinate{x,z};
        if(!started||coordinate!=current)
        {
            HlodCell cell;cell.coordinate={x,z};cell.parts.clear();
            candidate.cells.push_back(std::move(cell));current=coordinate;started=true;
        }
        candidate.cells.back().worldError=errors[coordinate];
        candidate.cells.back().parts.push_back({material,std::move(mesh)});
    }
    for(auto& cell:candidate.cells)
    {
        bool first=true;Vec3 minimum{},maximum{};
        for(const auto& part:cell.parts)for(const auto& vertex:part.mesh.vertices)
        {
            const auto position=ToVec3(vertex.position);
            if(first){minimum=maximum=position;first=false;continue;}
            minimum={ (std::min)(minimum.x,position.x),(std::min)(minimum.y,position.y),(std::min)(minimum.z,position.z) };
            maximum={ (std::max)(maximum.x,position.x),(std::max)(maximum.y,position.y),(std::max)(maximum.z,position.z) };
        }
        if(first)
        {
            Diagnostic(diagnostics,"Rendering.LodAssetInvalid","empty HLOD cell");
            return false;
        }
        cell.center=(minimum+maximum)*0.5f;
        float radius=0;
        for(const auto& part:cell.parts)for(const auto& vertex:part.mesh.vertices)
            radius=(std::max)(radius,Length(ToVec3(vertex.position)-cell.center));
        if(!(radius>0)||!std::isfinite(radius))
        {
            Diagnostic(diagnostics,"Rendering.LodAssetInvalid","HLOD radius");
            return false;
        }
        cell.radius=radius;
    }
    std::sort(candidate.cells.begin(),candidate.cells.end(),[](const HlodCell& a,const HlodCell& b)
    {
        return std::tie(a.coordinate.x,a.coordinate.z)<std::tie(b.coordinate.x,b.coordinate.z);
    });
    output=std::move(candidate);
    return true;
}
bool WriteHlodScene(const std::filesystem::path& path,const HlodScene& scene,std::string& error)
{
    if(scene.cells.empty()||scene.instanceCount==0||!(scene.cellSize>0)||!std::isfinite(scene.cellSize)
        ||scene.cells.size()>HlodMaximumCells){error="Rendering.LodAssetInvalid";return false;}
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(),HlodMagic,HlodMagic+4);
    AppendUint32(bytes,HlodFileVersion);AppendFloat(bytes,scene.cellSize);
    AppendUint32(bytes,scene.instanceCount);AppendUint32(bytes,static_cast<std::uint32_t>(scene.cells.size()));
    for(std::size_t index=0;index<scene.cells.size();++index)
    {
        const auto& cell=scene.cells[index];
        if(cell.parts.empty()||cell.parts.size()>HlodMaximumPartsPerCell||!(cell.radius>0)||!std::isfinite(cell.radius)
            ||!std::isfinite(cell.worldError)||cell.worldError<0||!Finite(cell.center)){error="Rendering.LodAssetInvalid";return false;}
        if(index&&!(std::tie(scene.cells[index-1].coordinate.x,scene.cells[index-1].coordinate.z)
            <std::tie(cell.coordinate.x,cell.coordinate.z))){error="Rendering.LodAssetInvalid";return false;}
        AppendUint32(bytes,static_cast<std::uint32_t>(cell.coordinate.x));
        AppendUint32(bytes,static_cast<std::uint32_t>(cell.coordinate.z));
        for(int axis=0;axis<3;++axis)AppendFloat(bytes,axis==0?cell.center.x:axis==1?cell.center.y:cell.center.z);
        AppendFloat(bytes,cell.radius);AppendFloat(bytes,cell.worldError);
        AppendUint32(bytes,static_cast<std::uint32_t>(cell.parts.size()));
        for(const auto& part:cell.parts)
        {
            if(part.material.size()>HlodMaximumMaterialKey||part.mesh.vertices.empty()
                ||part.mesh.indices.empty()||part.mesh.indices.size()%3!=0
                ||part.mesh.vertices.size()>0xFFFFFFFFull||part.mesh.indices.size()>0xFFFFFFFFull)
            {error="Rendering.LodAssetInvalid";return false;}
            AppendUint32(bytes,static_cast<std::uint32_t>(part.material.size()));
            bytes.insert(bytes.end(),part.material.begin(),part.material.end());
            AppendUint32(bytes,static_cast<std::uint32_t>(part.mesh.vertices.size()));
            AppendUint32(bytes,static_cast<std::uint32_t>(part.mesh.indices.size()));
            for(const auto& vertex:part.mesh.vertices)
            {
                for(const auto value:vertex.position)AppendFloat(bytes,value);
                for(const auto value:vertex.normal)AppendFloat(bytes,value);
                for(const auto value:vertex.uv)AppendFloat(bytes,value);
                for(const auto value:vertex.tangent)AppendFloat(bytes,value);
            }
            for(const auto value:part.mesh.indices)
            {
                if(value>=part.mesh.vertices.size()){error="Rendering.LodAssetInvalid";return false;}
                AppendUint32(bytes,value);
            }
        }
    }
    if(!asset_io::WriteAtomic(path,bytes,true)){error="Assets.WriteFailed";return false;}
    return true;
}
bool ReadHlodScene(const std::filesystem::path& path,HlodScene& output,std::string& error)
{
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)||bytes.size()<4+4*4){error="Assets.ReadFailed";return false;}
    if(std::memcmp(bytes.data(),HlodMagic,4)!=0)return false;
    const auto* cursor=bytes.data()+4;const auto* end=bytes.data()+bytes.size();
    const auto version=ReadUint32(cursor);cursor+=4;
    const auto cellSize=ReadFloat(cursor);cursor+=4;
    const auto instanceCount=ReadUint32(cursor);cursor+=4;
    const auto cellCount=ReadUint32(cursor);cursor+=4;
    if(version!=HlodFileVersion||!(cellSize>0)||!std::isfinite(cellSize)||instanceCount==0
        ||cellCount==0||cellCount>HlodMaximumCells)return false;
    HlodScene candidate;candidate.cellSize=cellSize;candidate.instanceCount=instanceCount;
    for(std::uint32_t cellIndex=0;cellIndex<cellCount;++cellIndex)
    {
        if(cursor+4*2+4*3+4*3>end)return false;
        HlodCell cell;
        cell.coordinate={static_cast<std::int32_t>(ReadUint32(cursor)),
            static_cast<std::int32_t>(ReadUint32(cursor+4))};cursor+=8;
        cell.center={ReadFloat(cursor),ReadFloat(cursor+4),ReadFloat(cursor+8)};cursor+=12;
        cell.radius=ReadFloat(cursor);cursor+=4;
        cell.worldError=ReadFloat(cursor);cursor+=4;
        const auto partCount=ReadUint32(cursor);cursor+=4;
        if(partCount==0||partCount>HlodMaximumPartsPerCell||!(cell.radius>0)||!std::isfinite(cell.radius)
            ||!(cell.worldError>=0)||!std::isfinite(cell.worldError)||!Finite(cell.center))return false;
        if(cellIndex&&!(std::tie(candidate.cells[cellIndex-1].coordinate.x,candidate.cells[cellIndex-1].coordinate.z)
            <std::tie(cell.coordinate.x,cell.coordinate.z)))return false;
        for(std::uint32_t partIndex=0;partIndex<partCount;++partIndex)
        {
            if(cursor+4>end)return false;
            const auto materialLength=ReadUint32(cursor);cursor+=4;
            if(materialLength>HlodMaximumMaterialKey||cursor+materialLength>end)return false;
            HlodPart part;
            part.material.assign(reinterpret_cast<const char*>(cursor),materialLength);cursor+=materialLength;
            if(cursor+8>end)return false;
            const auto vertexCount=ReadUint32(cursor);const auto indexCount=ReadUint32(cursor+4);cursor+=8;
            if(vertexCount==0||indexCount==0||indexCount%3!=0)return false;
            const std::uint64_t vertexBytes=std::uint64_t(vertexCount)*12*4;
            const std::uint64_t indexBytes=std::uint64_t(indexCount)*4;
            if(cursor+vertexBytes+indexBytes>end)return false;
            part.mesh.vertices.resize(vertexCount);
            for(auto& vertex:part.mesh.vertices)
            {
                for(auto& value:vertex.position){value=ReadFloat(cursor);cursor+=4;}
                for(auto& value:vertex.normal){value=ReadFloat(cursor);cursor+=4;}
                for(auto& value:vertex.uv){value=ReadFloat(cursor);cursor+=4;}
                for(auto& value:vertex.tangent){value=ReadFloat(cursor);cursor+=4;}
            }
            part.mesh.indices.resize(indexCount);
            for(auto& value:part.mesh.indices)
            {
                value=ReadUint32(cursor);cursor+=4;
                if(value>=vertexCount)return false;
            }
            cell.parts.push_back(std::move(part));
        }
        candidate.cells.push_back(std::move(cell));
    }
    if(cursor!=end)return false;
    output=std::move(candidate);return true;
}
}
