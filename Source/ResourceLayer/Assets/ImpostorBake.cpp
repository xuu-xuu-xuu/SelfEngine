#include "ImpostorBake.h"
#include "AssetIO.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace se
{
namespace
{
constexpr std::uint32_t ImpostorFileVersion=1;
constexpr std::uint32_t ImpostorMaximumTile=256;
constexpr char ImpostorMagic[4]={'S','E','I','M'};
void AppendUint32(std::vector<std::uint8_t>& bytes,std::uint32_t value)
{
    for(int shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>((value>>shift)&0xFF));
}
std::uint32_t ReadUint32(const std::uint8_t* bytes)
{
    std::uint32_t value=0;
    for(int shift=0;shift<32;shift+=8)value|=static_cast<std::uint32_t>(*bytes++)<<shift;
    return value;
}
void Diagnostic(std::vector<AssetDiagnostic>& diagnostics,const char* key,const char* detail)
{
    diagnostics.push_back({key,detail,false});
}
std::uint8_t Encode(float value)
{
    return static_cast<std::uint8_t>(std::clamp(value,0.0f,1.0f)*255.0f+0.5f);
}
}
std::size_t ImpostorAtlas::PixelOffset(std::uint32_t view,std::uint32_t x,std::uint32_t y) const
{
    const auto column=view%columns,row=view/columns;
    const auto atlasX=column*tileSize+x,atlasY=row*tileSize+y;
    return (static_cast<std::size_t>(atlasY)*Width()+atlasX)*4;
}
bool BakeImpostorAtlas(const MeshData& mesh,const Mat4& meshToWorld,const ImpostorBakeSettings& settings,
    ImpostorAtlas& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(mesh.vertices.empty()||mesh.indices.empty()||mesh.indices.size()%3!=0
        ||settings.tileSize<8||settings.tileSize>ImpostorMaximumTile
        ||!std::isfinite(settings.ambient)||settings.ambient<0||settings.ambient>1)
    {
        Diagnostic(diagnostics,"Rendering.LodAssetInvalid","impostor bake input");
        return false;
    }
    for(const auto index:mesh.indices)if(index>=mesh.vertices.size())return false;
    const auto inverse=Inverse(meshToWorld);
    const auto normalFromWorld=inverse?Transpose(*inverse):Mat4{};
    std::vector<Vec3> positions(mesh.vertices.size()),normals(mesh.vertices.size());
    Vec3 minimum{},maximum{};bool first=true;
    for(std::size_t index=0;index<mesh.vertices.size();++index)
    {
        positions[index]=TransformPoint(meshToWorld,{mesh.vertices[index].position[0],mesh.vertices[index].position[1],mesh.vertices[index].position[2]});
        auto normal=TransformVector(normalFromWorld,{mesh.vertices[index].normal[0],mesh.vertices[index].normal[1],mesh.vertices[index].normal[2]});
        const auto length=Length(normal);
        normals[index]=length>1e-8f?normal/length:Vec3{0,1,0};
        if(first){minimum=maximum=positions[index];first=false;continue;}
        minimum={ (std::min)(minimum.x,positions[index].x),(std::min)(minimum.y,positions[index].y),(std::min)(minimum.z,positions[index].z) };
        maximum={ (std::max)(maximum.x,positions[index].x),(std::max)(maximum.y,positions[index].y),(std::max)(maximum.z,positions[index].z) };
    }
    if(first)return false;
    const auto center=(minimum+maximum)*0.5f;
    float radius=0;
    for(const auto& position:positions)radius=(std::max)(radius,Length(position-center));
    if(!(radius>0)||!std::isfinite(radius))
    {
        Diagnostic(diagnostics,"Rendering.LodAssetInvalid","impostor radius");
        return false;
    }
    const auto light=Normalize(settings.lightDirection);
    const auto size=static_cast<int>(settings.tileSize);
    const float extent=radius*1.05f;
    ImpostorAtlas atlas;
    atlas.tileSize=settings.tileSize;atlas.columns=4;atlas.rows=4;
    atlas.rgba.assign(static_cast<std::size_t>(atlas.Width())*atlas.Height()*4,0);
    std::vector<std::uint8_t> tile(static_cast<std::size_t>(size)*size*4,0);
    std::vector<float> depth(static_cast<std::size_t>(size)*size,-std::numeric_limits<float>::max());
    for(std::uint32_t view=0;view<ImpostorViewCount;++view)
    {
        const float azimuth=float(view)*2.0f*Pi/float(ImpostorViewCount);
        const Vec3 cameraDirection{std::sin(azimuth),0,std::cos(azimuth)};
        const auto right=Normalize(Cross(Vec3{0,1,0},cameraDirection));
        const auto up=Cross(cameraDirection,right);
        std::fill(tile.begin(),tile.end(),std::uint8_t{0});
        std::fill(depth.begin(),depth.end(),-std::numeric_limits<float>::max());
        for(std::size_t triangle=0;triangle+2<mesh.indices.size();triangle+=3)
        {
            std::array<float,3> px{},py{},pz{},shade{};
            for(int corner=0;corner<3;++corner)
            {
                const auto index=mesh.indices[triangle+static_cast<std::size_t>(corner)];
                const auto offset=positions[index]-center;
                const float x=Dot(offset,right),y=Dot(offset,up),z=Dot(offset,cameraDirection);
                px[static_cast<std::size_t>(corner)]=(x+extent)/(2*extent)*float(size);
                py[static_cast<std::size_t>(corner)]=(extent-y)/(2*extent)*float(size);
                pz[static_cast<std::size_t>(corner)]=z;
                const float diffuse=(std::max)(Dot(normals[index],light),0.0f);
                shade[static_cast<std::size_t>(corner)]=settings.ambient+(1-settings.ambient)*diffuse;
            }
            int minimumX=size,minimumY=size,maximumX=-1,maximumY=-1;
            for(int corner=0;corner<3;++corner)
            {
                minimumX=(std::min)(minimumX,static_cast<int>(std::floor(px[corner])));
                minimumY=(std::min)(minimumY,static_cast<int>(std::floor(py[corner])));
                maximumX=(std::max)(maximumX,static_cast<int>(std::ceil(px[corner])));
                maximumY=(std::max)(maximumY,static_cast<int>(std::ceil(py[corner])));
            }
            minimumX=(std::max)(minimumX,0);minimumY=(std::max)(minimumY,0);
            maximumX=(std::min)(maximumX,size-1);maximumY=(std::min)(maximumY,size-1);
            for(int y=minimumY;y<=maximumY;++y)for(int x=minimumX;x<=maximumX;++x)
            {
                const float sampleX=float(x)+0.5f,sampleY=float(y)+0.5f;
                const auto edge=[](float ax,float ay,float bx,float by,float sx,float sy)
                {return (sx-ax)*(by-ay)-(sy-ay)*(bx-ax);};
                const float e0=edge(px[1],py[1],px[2],py[2],sampleX,sampleY);
                const float e1=edge(px[2],py[2],px[0],py[0],sampleX,sampleY);
                const float e2=edge(px[0],py[0],px[1],py[1],sampleX,sampleY);
                const float sum=e0+e1+e2;
                if(std::abs(sum)<1e-8f)continue;
                const float w0=e0/sum,w1=e1/sum,w2=e2/sum;
                if(w0<0||w1<0||w2<0)continue;
                const float z=w0*pz[0]+w1*pz[1]+w2*pz[2];
                const auto pixel=static_cast<std::size_t>(y)*size+x;
                if(z<=depth[pixel])continue;
                depth[pixel]=z;
                const float value=std::clamp(w0*shade[0]+w1*shade[1]+w2*shade[2],0.0f,1.0f);
                tile[pixel*4+0]=Encode(settings.color.x*value);
                tile[pixel*4+1]=Encode(settings.color.y*value);
                tile[pixel*4+2]=Encode(settings.color.z*value);
                tile[pixel*4+3]=255;
            }
        }
        for(int y=0;y<size;++y)for(int x=0;x<size;++x)
        {
            const auto source=(static_cast<std::size_t>(y)*size+x)*4;
            const auto target=atlas.PixelOffset(view,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y));
            std::memcpy(atlas.rgba.data()+target,tile.data()+source,4);
        }
    }
    output=std::move(atlas);
    return true;
}
std::uint32_t ImpostorViewIndex(float azimuthRadians)
{
    if(!std::isfinite(azimuthRadians))return 0;
    const float step=2.0f*Pi/float(ImpostorViewCount);
    float value=std::fmod(azimuthRadians,2.0f*Pi);
    if(value<0)value+=2.0f*Pi;
    const auto index=static_cast<std::uint32_t>(std::lround(value/step));
    return index%ImpostorViewCount;
}
void ImpostorViewPair(float azimuthRadians,std::uint32_t& first,std::uint32_t& second,float& blend)
{
    if(!std::isfinite(azimuthRadians)){first=second=0;blend=0;return;}
    const float step=2.0f*Pi/float(ImpostorViewCount);
    float value=std::fmod(azimuthRadians,2.0f*Pi);
    if(value<0)value+=2.0f*Pi;
    const auto scaled=value/step;
    const auto base=static_cast<std::uint32_t>(std::floor(scaled));
    first=base%ImpostorViewCount;
    second=(first+1)%ImpostorViewCount;
    blend=std::clamp(scaled-float(base),0.0f,1.0f);
}
bool WriteImpostorAtlas(const std::filesystem::path& path,const ImpostorAtlas& atlas,std::string& error)
{
    const auto expected=static_cast<std::size_t>(atlas.Width())*atlas.Height()*4;
    if(atlas.tileSize<8||atlas.tileSize>ImpostorMaximumTile||atlas.columns==0||atlas.rows==0
        ||atlas.columns*atlas.rows<ImpostorViewCount||atlas.rgba.size()!=expected)
    {error="Rendering.LodAssetInvalid";return false;}
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(),ImpostorMagic,ImpostorMagic+4);
    AppendUint32(bytes,ImpostorFileVersion);AppendUint32(bytes,atlas.tileSize);
    AppendUint32(bytes,atlas.columns);AppendUint32(bytes,atlas.rows);
    bytes.insert(bytes.end(),atlas.rgba.begin(),atlas.rgba.end());
    if(!asset_io::WriteAtomic(path,bytes,true)){error="Assets.WriteFailed";return false;}
    return true;
}
bool ReadImpostorAtlas(const std::filesystem::path& path,ImpostorAtlas& output,std::string& error)
{
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)||bytes.size()<4+4*4){error="Assets.ReadFailed";return false;}
    if(std::memcmp(bytes.data(),ImpostorMagic,4)!=0)return false;
    const auto* cursor=bytes.data()+4;
    const auto version=ReadUint32(cursor);cursor+=4;
    const auto tileSize=ReadUint32(cursor);cursor+=4;
    const auto columns=ReadUint32(cursor);cursor+=4;
    const auto rows=ReadUint32(cursor);cursor+=4;
    if(version!=ImpostorFileVersion||tileSize<8||tileSize>ImpostorMaximumTile||columns==0||rows==0
        ||columns*rows<ImpostorViewCount)return false;
    const auto expected=static_cast<std::size_t>(columns)*tileSize*rows*tileSize*4;
    if(static_cast<std::size_t>(bytes.data()+bytes.size()-cursor)!=expected)return false;
    ImpostorAtlas atlas;
    atlas.tileSize=tileSize;atlas.columns=columns;atlas.rows=rows;
    atlas.rgba.assign(cursor,cursor+expected);
    output=std::move(atlas);return true;
}
}
