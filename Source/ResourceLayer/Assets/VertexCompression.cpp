#include "VertexCompression.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace se
{
namespace
{
float Clamp01(float value){return value<0?0:value>1?1:value;}

std::uint16_t QuantizeUnorm16(float value)
{
    return static_cast<std::uint16_t>(std::lround(Clamp01(value)*65535.0f));
}

std::uint8_t QuantizeUnorm8(float value)
{
    return static_cast<std::uint8_t>(std::lround(Clamp01(value)*255.0f));
}

std::uint16_t FloatToHalf(float value)
{
    std::uint32_t bits=0;
    std::memcpy(&bits,&value,sizeof(bits));
    const std::uint32_t sign=(bits>>16)&0x8000u;
    std::int32_t exponent=static_cast<std::int32_t>((bits>>23)&0xFFu)-127+15;
    std::uint32_t mantissa=bits&0x7FFFFFu;
    if(exponent<=0)
    {
        if(exponent<-10)return static_cast<std::uint16_t>(sign);
        mantissa=(mantissa|0x800000u)>>(1-exponent);
        return static_cast<std::uint16_t>(sign+(mantissa>>13));
    }
    if(exponent>=31)return static_cast<std::uint16_t>(sign|0x7C00u);
    return static_cast<std::uint16_t>(sign|(static_cast<std::uint32_t>(exponent)<<10)|(mantissa>>13));
}

float HalfToFloat(std::uint16_t value)
{
    const std::uint32_t sign=(value&0x8000u)<<16;
    std::uint32_t exponent=(value>>10)&0x1Fu;
    std::uint32_t mantissa=value&0x3FFu;
    std::uint32_t bits=0;
    if(exponent==0)
    {
        if(mantissa==0)bits=sign;
        else
        {
            exponent=127-15+1;
            while((mantissa&0x400u)==0){mantissa<<=1;--exponent;}
            mantissa&=0x3FFu;
            bits=sign|(exponent<<23)|(mantissa<<13);
        }
    }
    else if(exponent==31)bits=sign|0x7F800000u|(mantissa<<13);
    else bits=sign|((exponent-15+127)<<23)|(mantissa<<13);
    float result=0;
    std::memcpy(&result,&bits,sizeof(result));
    return result;
}

Vec3 NormalizeDirection(Vec3 value)
{
    const float length=std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
    if(length<=1e-8f)return {0,1,0};
    return {value.x/length,value.y/length,value.z/length};
}

void EncodeOctahedral(Vec3 normal,std::uint8_t output[2])
{
    normal=NormalizeDirection(normal);
    float x=normal.x,y=normal.y,z=normal.z;
    const float l1=std::fabs(x)+std::fabs(y)+std::fabs(z);
    if(l1>1e-8f){x/=l1;y/=l1;z/=l1;}
    if(z<0)
    {
        const float oldX=x;
        x=(1.0f-std::fabs(y))*(oldX<0?-1.0f:1.0f);
        y=(1.0f-std::fabs(oldX))*(y<0?-1.0f:1.0f);
    }
    output[0]=QuantizeUnorm8(x*0.5f+0.5f);
    output[1]=QuantizeUnorm8(y*0.5f+0.5f);
}

Vec3 DecodeOctahedral(const std::uint8_t input[2])
{
    float x=float(input[0])/255.0f*2.0f-1.0f;
    float y=float(input[1])/255.0f*2.0f-1.0f;
    float z=1.0f-std::fabs(x)-std::fabs(y);
    if(z<0)
    {
        const float oldX=x;
        x=(1.0f-std::fabs(y))*(oldX<0?-1.0f:1.0f);
        y=(1.0f-std::fabs(oldX))*(y<0?-1.0f:1.0f);
    }
    return NormalizeDirection({x,y,z});
}
}

bool CompressMeshVertices(std::span<const MeshVertex> vertices,const Aabb& bounds,
    std::vector<PackedVertex>& output,MeshQuantization& quantization,std::string& error)
{
    if(vertices.empty()){error="Rendering.VertexQuantizeFailed";return false;}
    const std::array<float,3> minimum{bounds.minimum.x,bounds.minimum.y,bounds.minimum.z};
    std::array<float,3> extent{
        bounds.maximum.x-bounds.minimum.x,
        bounds.maximum.y-bounds.minimum.y,
        bounds.maximum.z-bounds.minimum.z};
    for(float& value:extent)if(!(value>1e-8f))value=1.0f;
    // 渲染端把量化折进世界矩阵，必须是均匀缩放，否则法线会被拉斜。
    const float uniformExtent=(std::max)({extent[0],extent[1],extent[2]});
    extent={uniformExtent,uniformExtent,uniformExtent};
    output.clear();output.reserve(vertices.size());
    for(const auto& vertex:vertices)
    {
        PackedVertex packed;
        for(int axis=0;axis<3;++axis)
        {
            const float normalized=(vertex.position[axis]-minimum[axis])/extent[axis];
            packed.position[axis]=QuantizeUnorm16(normalized);
        }
        EncodeOctahedral({vertex.normal[0],vertex.normal[1],vertex.normal[2]},packed.normal);
        packed.uv[0]=FloatToHalf(vertex.uv[0]);
        packed.uv[1]=FloatToHalf(vertex.uv[1]);
        const Vec3 tangent=NormalizeDirection({vertex.tangent[0],vertex.tangent[1],vertex.tangent[2]});
        const std::uint8_t tx=QuantizeUnorm8(tangent.x*0.5f+0.5f);
        const std::uint8_t ty=QuantizeUnorm8(tangent.y*0.5f+0.5f);
        const std::uint8_t tz=QuantizeUnorm8(tangent.z*0.5f+0.5f);
        const std::uint8_t tw=vertex.tangent[3]<0?0u:255u;
        packed.tangent=std::uint32_t(tx)|(std::uint32_t(ty)<<8)|(std::uint32_t(tz)<<16)|(std::uint32_t(tw)<<24);
        output.push_back(packed);
    }
    quantization.minimum=minimum;
    quantization.extent=extent;
    return true;
}

Vec3 DecodePackedNormal(const PackedVertex& vertex)
{
    return DecodeOctahedral(vertex.normal);
}

Vec3 DecodePackedPosition(const PackedVertex& vertex,const MeshQuantization& quantization)
{
    return {
        quantization.minimum[0]+float(vertex.position[0])/65535.0f*quantization.extent[0],
        quantization.minimum[1]+float(vertex.position[1])/65535.0f*quantization.extent[1],
        quantization.minimum[2]+float(vertex.position[2])/65535.0f*quantization.extent[2]};
}

void DecodePackedUv(const PackedVertex& vertex,float& u,float& v)
{
    u=HalfToFloat(vertex.uv[0]);
    v=HalfToFloat(vertex.uv[1]);
}
}
