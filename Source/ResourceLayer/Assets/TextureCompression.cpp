#include "TextureCompression.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace se
{
namespace
{
std::uint16_t To565(int r,int g,int b)
{
    return static_cast<std::uint16_t>(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
}

std::array<int,3> From565(std::uint16_t value)
{
    const int r=((value>>11)&0x1F)<<3;
    const int g=((value>>5)&0x3F)<<2;
    const int b=(value&0x1F)<<3;
    return {r|(r>>5),g|(g>>6),b|(b>>5)};
}

int Distance(const std::array<int,3>& a,const std::array<int,3>& b)
{
    const int dr=a[0]-b[0],dg=a[1]-b[1],db=a[2]-b[2];
    return dr*dr+dg*dg+db*db;
}
// BC4 单通道块：a0/a1 端点 + 16 个 3 位索引（48 位）。编码统一用 8 值模式（a0>a1）。
void EncodeBc4Channel(const std::uint8_t values[16],std::uint8_t* block)
{
    std::uint8_t minimum=values[0],maximum=values[0];
    for(int index=1;index<16;++index)
    {
        minimum=(std::min)(minimum,values[index]);
        maximum=(std::max)(maximum,values[index]);
    }
    block[0]=maximum;block[1]=minimum;
    if(maximum==minimum)
    {
        for(int index=2;index<8;++index)block[index]=0;
        return;
    }
    std::uint8_t palette[8]{maximum,minimum};
    // 与 DecodeBc4Channel 使用同一截断公式，保证编码器选择的索引在解码端完全复现。
    for(int index=2;index<8;++index)
        palette[index]=static_cast<std::uint8_t>(((8-index)*int(maximum)+(index-1)*int(minimum))/7);
    std::uint64_t indices=0;
    for(int texel=0;texel<16;++texel)
    {
        int best=0,bestDistance=std::abs(int(values[texel])-int(palette[0]));
        for(int candidate=1;candidate<8;++candidate)
        {
            const int distance=std::abs(int(values[texel])-int(palette[candidate]));
            if(distance<bestDistance){bestDistance=distance;best=candidate;}
        }
        indices|=std::uint64_t(best)<<(texel*3);
    }
    for(int index=0;index<6;++index)block[2+index]=static_cast<std::uint8_t>((indices>>(index*8))&0xFF);
}
void DecodeBc4Channel(const std::uint8_t* block,std::uint8_t values[16])
{
    const int endpoint0=block[0],endpoint1=block[1];
    int palette[8]{endpoint0,endpoint1};
    if(endpoint0>endpoint1)
        for(int index=2;index<8;++index)palette[index]=((8-index)*endpoint0+(index-1)*endpoint1)/7;
    else
    {
        for(int index=2;index<6;++index)palette[index]=((6-index)*endpoint0+(index-1)*endpoint1)/5;
        palette[6]=0;palette[7]=255;
    }
    std::uint64_t indices=0;
    for(int index=0;index<6;++index)indices|=std::uint64_t(block[2+index])<<(index*8);
    for(int texel=0;texel<16;++texel)values[texel]=static_cast<std::uint8_t>(palette[(indices>>(texel*3))&7]);
}
// 2x2 盒式降采样；链式压缩共用，保证 BC1/BC5 的 mip 内容一致。
void DownsampleHalf(std::span<const std::uint8_t> source,std::uint32_t width,std::uint32_t height,
    std::vector<std::uint8_t>& next,std::uint32_t& nextWidth,std::uint32_t& nextHeight)
{
    nextWidth=(std::max)(1u,width/2);nextHeight=(std::max)(1u,height/2);
    next.assign(std::size_t(nextWidth)*nextHeight*4,0);
    for(std::uint32_t y=0;y<nextHeight;++y)for(std::uint32_t x=0;x<nextWidth;++x)
    {
        std::uint32_t sums[4]{};
        for(std::uint32_t dy=0;dy<2;++dy)for(std::uint32_t dx=0;dx<2;++dx)
        {
            const std::uint32_t sx=(std::min)(width-1,x*2+dx),sy=(std::min)(height-1,y*2+dy);
            const auto* pixel=source.data()+(std::size_t(sy)*width+sx)*4;
            for(int channel=0;channel<4;++channel)sums[channel]+=pixel[channel];
        }
        auto* pixel=next.data()+(std::size_t(y)*nextWidth+x)*4;
        for(int channel=0;channel<4;++channel)pixel[channel]=static_cast<std::uint8_t>(sums[channel]/4);
    }
}
}

bool CompressBc1(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    Bc1Image& output,std::string& error)
{
    if(!width||!height||rgba8.size()!=std::size_t(width)*height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    const std::uint32_t blocksX=(width+3)/4,blocksY=(height+3)/4;
    output.width=width;output.height=height;
    output.blocks.assign(std::size_t(blocksX)*blocksY*8,0);
    auto pixel=[&](std::uint32_t x,std::uint32_t y)->const std::uint8_t*
    {
        x=(std::min)(x,width-1);y=(std::min)(y,height-1);
        return rgba8.data()+(std::size_t(y)*width+x)*4;
    };
    std::vector<std::array<int,3>> colors(16);
    for(std::uint32_t by=0;by<blocksY;++by)for(std::uint32_t bx=0;bx<blocksX;++bx)
    {
        for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<4;++x)
        {
            const auto* p=pixel(bx*4+x,by*4+y);
            colors[y*4+x]={p[0],p[1],p[2]};
        }
        std::array<int,3> minimum=colors[0],maximum=colors[0];
        for(const auto& color:colors)for(int c=0;c<3;++c)
        {
            minimum[c]=(std::min)(minimum[c],color[c]);
            maximum[c]=(std::max)(maximum[c],color[c]);
        }
        std::uint16_t c0=To565(maximum[0],maximum[1],maximum[2]);
        std::uint16_t c1=To565(minimum[0],minimum[1],minimum[2]);
        if(c0<c1)std::swap(c0,c1);
        const auto a=From565(c0),b=From565(c1);
        const std::array<std::array<int,3>,4> palette{
            a,b,{(2*a[0]+b[0])/3,(2*a[1]+b[1])/3,(2*a[2]+b[2])/3},
            {(a[0]+2*b[0])/3,(a[1]+2*b[1])/3,(a[2]+2*b[2])/3}};
        std::uint32_t indices=0;
        for(int i=0;i<16;++i)
        {
            int best=0,bestDistance=Distance(colors[i],palette[0]);
            for(int candidate=1;candidate<4;++candidate)
            {
                const int distance=Distance(colors[i],palette[candidate]);
                if(distance<bestDistance){bestDistance=distance;best=candidate;}
            }
            indices|=static_cast<std::uint32_t>(best)<<(i*2);
        }
        auto* block=output.blocks.data()+(std::size_t(by)*blocksX+bx)*8;
        block[0]=static_cast<std::uint8_t>(c0&0xFF);block[1]=static_cast<std::uint8_t>(c0>>8);
        block[2]=static_cast<std::uint8_t>(c1&0xFF);block[3]=static_cast<std::uint8_t>(c1>>8);
        std::memcpy(block+4,&indices,4);
    }
    return true;
}

bool CompressBc1MipChain(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    std::uint32_t levels,std::vector<Bc1Image>& output,std::string& error)
{
    if(!width||!height||!levels||rgba8.size()!=std::size_t(width)*height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    output.clear();
    std::vector<std::uint8_t> source(rgba8.begin(),rgba8.end());
    std::uint32_t currentWidth=width,currentHeight=height;
    for(std::uint32_t level=0;level<levels;++level)
    {
        Bc1Image image;
        if(!CompressBc1(source,currentWidth,currentHeight,image,error))return false;
        output.push_back(std::move(image));
        if(currentWidth==1&&currentHeight==1)break;
        std::vector<std::uint8_t> next;std::uint32_t nextWidth=0,nextHeight=0;
        DownsampleHalf(source,currentWidth,currentHeight,next,nextWidth,nextHeight);
        source=std::move(next);currentWidth=nextWidth;currentHeight=nextHeight;
    }
    return true;
}
bool CompressBc5(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    Bc5Image& output,std::string& error)
{
    if(!width||!height||rgba8.size()!=std::size_t(width)*height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    const std::uint32_t blocksX=(width+3)/4,blocksY=(height+3)/4;
    output.width=width;output.height=height;
    output.blocks.assign(std::size_t(blocksX)*blocksY*16,0);
    auto pixel=[&](std::uint32_t x,std::uint32_t y)->const std::uint8_t*
    {
        x=(std::min)(x,width-1);y=(std::min)(y,height-1);
        return rgba8.data()+(std::size_t(y)*width+x)*4;
    };
    std::uint8_t red[16],green[16];
    for(std::uint32_t by=0;by<blocksY;++by)for(std::uint32_t bx=0;bx<blocksX;++bx)
    {
        for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<4;++x)
        {
            const auto* value=pixel(bx*4+x,by*4+y);
            red[y*4+x]=value[0];green[y*4+x]=value[1];
        }
        auto* block=output.blocks.data()+(std::size_t(by)*blocksX+bx)*16;
        EncodeBc4Channel(red,block);
        EncodeBc4Channel(green,block+8);
    }
    return true;
}
bool CompressBc5MipChain(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    std::uint32_t levels,std::vector<Bc5Image>& output,std::string& error)
{
    if(!width||!height||!levels||rgba8.size()!=std::size_t(width)*height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    output.clear();
    std::vector<std::uint8_t> source(rgba8.begin(),rgba8.end());
    std::uint32_t currentWidth=width,currentHeight=height;
    for(std::uint32_t level=0;level<levels;++level)
    {
        Bc5Image image;
        if(!CompressBc5(source,currentWidth,currentHeight,image,error))return false;
        output.push_back(std::move(image));
        if(currentWidth==1&&currentHeight==1)break;
        std::vector<std::uint8_t> next;std::uint32_t nextWidth=0,nextHeight=0;
        DownsampleHalf(source,currentWidth,currentHeight,next,nextWidth,nextHeight);
        source=std::move(next);currentWidth=nextWidth;currentHeight=nextHeight;
    }
    return true;
}
void DecodeBc5(const Bc5Image& image,std::vector<std::uint8_t>& rgba8)
{
    rgba8.assign(std::size_t(image.width)*image.height*4,0);
    for(std::size_t index=3;index<rgba8.size();index+=4)rgba8[index]=255;
    const std::uint32_t blocksX=(image.width+3)/4;
    std::uint8_t red[16],green[16];
    for(std::uint32_t by=0;by<(image.height+3)/4;++by)for(std::uint32_t bx=0;bx<blocksX;++bx)
    {
        const auto* block=image.blocks.data()+(std::size_t(by)*blocksX+bx)*16;
        DecodeBc4Channel(block,red);DecodeBc4Channel(block+8,green);
        for(int texel=0;texel<16;++texel)
        {
            const int x=bx*4+texel%4,y=by*4+texel/4;
            if(x>=static_cast<int>(image.width)||y>=static_cast<int>(image.height))continue;
            auto* pixel=rgba8.data()+(std::size_t(y)*image.width+x)*4;
            pixel[0]=red[texel];pixel[1]=green[texel];
        }
    }
}
bool CompressBc4(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    Bc4Image& output,std::string& error)
{
    if(!width||!height||rgba8.size()!=std::size_t(width)*height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    const std::uint32_t blocksX=(width+3)/4,blocksY=(height+3)/4;
    output.width=width;output.height=height;
    output.blocks.assign(std::size_t(blocksX)*blocksY*8,0);
    auto pixel=[&](std::uint32_t x,std::uint32_t y)->const std::uint8_t*
    {
        x=(std::min)(x,width-1);y=(std::min)(y,height-1);
        return rgba8.data()+(std::size_t(y)*width+x)*4;
    };
    std::uint8_t red[16];
    for(std::uint32_t by=0;by<blocksY;++by)for(std::uint32_t bx=0;bx<blocksX;++bx)
    {
        for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<4;++x)red[y*4+x]=pixel(bx*4+x,by*4+y)[0];
        EncodeBc4Channel(red,output.blocks.data()+(std::size_t(by)*blocksX+bx)*8);
    }
    return true;
}
void DecodeBc4(const Bc4Image& image,std::vector<std::uint8_t>& rgba8)
{
    rgba8.assign(std::size_t(image.width)*image.height*4,0);
    for(std::size_t index=3;index<rgba8.size();index+=4)rgba8[index]=255;
    const std::uint32_t blocksX=(image.width+3)/4;
    std::uint8_t red[16];
    for(std::uint32_t by=0;by<(image.height+3)/4;++by)for(std::uint32_t bx=0;bx<blocksX;++bx)
    {
        DecodeBc4Channel(image.blocks.data()+(std::size_t(by)*blocksX+bx)*8,red);
        for(int texel=0;texel<16;++texel)
        {
            const int x=bx*4+texel%4,y=by*4+texel/4;
            if(x>=static_cast<int>(image.width)||y>=static_cast<int>(image.height))continue;
            rgba8[(std::size_t(y)*image.width+x)*4]=red[texel];
        }
    }
}
bool CompressBc4MipChain(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    std::uint32_t levels,std::vector<Bc4Image>& output,std::string& error)
{
    if(!width||!height||!levels||rgba8.size()!=std::size_t(width)*height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    output.clear();
    std::vector<std::uint8_t> source(rgba8.begin(),rgba8.end());
    std::uint32_t currentWidth=width,currentHeight=height;
    for(std::uint32_t level=0;level<levels;++level)
    {
        Bc4Image image;
        if(!CompressBc4(source,currentWidth,currentHeight,image,error))return false;
        output.push_back(std::move(image));
        if(currentWidth==1&&currentHeight==1)break;
        std::vector<std::uint8_t> next;std::uint32_t nextWidth=0,nextHeight=0;
        DownsampleHalf(source,currentWidth,currentHeight,next,nextWidth,nextHeight);
        source=std::move(next);currentWidth=nextWidth;currentHeight=nextHeight;
    }
    return true;
}
void DecodeBc1(const Bc1Image& image,std::vector<std::uint8_t>& rgba8)
{
    rgba8.assign(std::size_t(image.width)*image.height*4,255);
    const std::uint32_t blocksX=(image.width+3)/4;
    for(std::uint32_t by=0;by<(image.height+3)/4;++by)for(std::uint32_t bx=0;bx<blocksX;++bx)
    {
        const auto* block=image.blocks.data()+(std::size_t(by)*blocksX+bx)*8;
        const std::uint16_t c0=static_cast<std::uint16_t>(block[0]|(block[1]<<8));
        const std::uint16_t c1=static_cast<std::uint16_t>(block[2]|(block[3]<<8));
        std::uint32_t indices=0;std::memcpy(&indices,block+4,4);
        const auto a=From565(c0),b=From565(c1);
        const std::array<std::array<int,3>,4> palette{
            a,b,{(2*a[0]+b[0])/3,(2*a[1]+b[1])/3,(2*a[2]+b[2])/3},
            {(a[0]+2*b[0])/3,(a[1]+2*b[1])/3,(a[2]+2*b[2])/3}};
        for(int i=0;i<16;++i)
        {
            const int x=bx*4+i%4,y=by*4+i/4;
            if(x>=static_cast<int>(image.width)||y>=static_cast<int>(image.height))continue;
            const auto& color=palette[(indices>>(i*2))&3];
            auto* pixel=rgba8.data()+(std::size_t(y)*image.width+x)*4;
            pixel[0]=static_cast<std::uint8_t>(color[0]);
            pixel[1]=static_cast<std::uint8_t>(color[1]);
            pixel[2]=static_cast<std::uint8_t>(color[2]);
            pixel[3]=255;
        }
    }
}
}
