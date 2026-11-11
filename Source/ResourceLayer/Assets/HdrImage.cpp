#include "HdrImage.h"
#include "AssetIO.h"
#include <algorithm>
#include <sstream>

namespace se
{
namespace
{
bool Valid(const FloatImage& image)
{
    if(!image.width||!image.height||image.width>16384||image.height>16384||std::uint64_t(image.width)*image.height>16*1024*1024
        ||image.pixels.size()!=size_t(image.width)*image.height)return false;
    return std::all_of(image.pixels.begin(),image.pixels.end(),[](Vec3 value){return IsFinite(value)&&value.x>=0&&value.y>=0&&value.z>=0;});
}
bool Fail(std::vector<AssetDiagnostic>& diagnostics,const char* key="Environment.HdrInvalid")
{asset_io::Diagnostic(diagnostics,key);return false;}
}
bool DecodeRadiance(std::span<const std::uint8_t> bytes,FloatImage& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(bytes.size()>asset_io::MaxFileBytes)return Fail(diagnostics);
    size_t cursor=0;
    const auto line=[&](std::string& text){
        text.clear();while(cursor<bytes.size()&&text.size()<4096)
        {const auto c=bytes[cursor++];if(c=='\n'){if(!text.empty()&&text.back()=='\r')text.pop_back();return true;}if(!c)return false;text.push_back(static_cast<char>(c));}return false;};
    std::string text;if(!line(text)||(text!="#?RADIANCE"&&text!="#?RGBE"))return Fail(diagnostics);
    bool format=false,headerEnd=false;
    while(cursor<bytes.size()&&cursor<65536)
    {
        if(!line(text))return Fail(diagnostics);
        if(text.empty()){headerEnd=true;break;}
        if(text.starts_with("FORMAT=")){if(format||text!="FORMAT=32-bit_rle_rgbe")return Fail(diagnostics);format=true;}
        // 不静默忽略会改变色彩/数值解释的元数据。
        if(text.starts_with("EXPOSURE=")||text.starts_with("COLORCORR=")||text.starts_with("PRIMARIES="))return Fail(diagnostics,"Environment.HdrMetadataUnsupported");
    }
    if(!format||!headerEnd||!line(text))return Fail(diagnostics);
    std::istringstream resolution(text);std::string yAxis,xAxis,extra;FloatImage image;
    if(!(resolution>>yAxis>>image.height>>xAxis>>image.width)||(resolution>>extra)||yAxis!="-Y"||xAxis!="+X")return Fail(diagnostics,"Environment.HdrOrientationUnsupported");
    if(!image.width||!image.height||image.width>16384||image.height>16384||std::uint64_t(image.width)*image.height>16*1024*1024)return Fail(diagnostics);
    image.pixels.resize(size_t(image.width)*image.height);std::vector<std::uint8_t> scanline(size_t(image.width)*4);
    for(std::uint32_t row=0;row<image.height;++row)
    {
        const bool rle=image.width>=8&&cursor+4<=bytes.size()&&bytes[cursor]==2&&bytes[cursor+1]==2&&!(bytes[cursor+2]&128);
        if(rle)
        {
            const auto length=(std::uint32_t(bytes[cursor+2])<<8)|bytes[cursor+3];cursor+=4;if(length!=image.width)return Fail(diagnostics);
            for(size_t channel=0;channel<4;++channel)
            {
                std::uint32_t column=0;
                while(column<image.width)
                {
                    if(cursor>=bytes.size())return Fail(diagnostics);const auto code=bytes[cursor++];if(!code)return Fail(diagnostics);
                    const auto count=std::uint32_t(code>128?code-128:code);if(count>image.width-column)return Fail(diagnostics);
                    if(code>128)
                    {
                        if(cursor>=bytes.size())return Fail(diagnostics);const auto value=bytes[cursor++];
                        for(std::uint32_t index=0;index<count;++index)scanline[size_t(column+index)*4+channel]=value;
                    }
                    else
                    {
                        if(count>bytes.size()-cursor)return Fail(diagnostics);
                        for(std::uint32_t index=0;index<count;++index)scanline[size_t(column+index)*4+channel]=bytes[cursor++];
                    }
                    column+=count;
                }
            }
        }
        else
        {
            if(scanline.size()>bytes.size()-cursor)return Fail(diagnostics);
            std::copy_n(bytes.begin()+cursor,scanline.size(),scanline.begin());cursor+=scanline.size();
            for(size_t index=0;index<scanline.size();index+=4)if(scanline[index]==1&&scanline[index+1]==1&&scanline[index+2]==1)return Fail(diagnostics,"Environment.HdrLegacyRleUnsupported");
        }
        for(std::uint32_t column=0;column<image.width;++column)
        {
            const auto* value=scanline.data()+size_t(column)*4;const float factor=value[3]?std::ldexp(1.0f,int(value[3])-136):0;
            image.pixels[size_t(row)*image.width+column]={value[0]*factor,value[1]*factor,value[2]*factor};
        }
    }
    if(cursor!=bytes.size()||!Valid(image))return Fail(diagnostics);
    output=std::move(image);return true;
}
bool LoadRadiance(const std::filesystem::path& path,FloatImage& output,std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path));return false;}
    return DecodeRadiance(bytes,output,diagnostics);
}
bool WriteRadiance(const std::filesystem::path& path,const FloatImage& image,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!Valid(image))return Fail(diagnostics);
    const auto header="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y "+std::to_string(image.height)+" +X "+std::to_string(image.width)+"\n";
    std::vector<std::uint8_t> bytes(header.begin(),header.end()),scanline(size_t(image.width)*4);
    for(std::uint32_t row=0;row<image.height;++row)
    {
        for(std::uint32_t column=0;column<image.width;++column)
        {
            const auto pixel=image.pixels[size_t(row)*image.width+column];const float largest=std::max({pixel.x,pixel.y,pixel.z});auto* target=scanline.data()+size_t(column)*4;
            if(largest<1e-32f){std::fill_n(target,4,std::uint8_t{});continue;}
            int exponent=0;std::frexp(largest,&exponent);if(exponent>127||exponent<-127)return Fail(diagnostics);
            const double scale=std::ldexp(1.0,8-exponent);
            target[0]=static_cast<std::uint8_t>(std::min(255.0,double(pixel.x)*scale));target[1]=static_cast<std::uint8_t>(std::min(255.0,double(pixel.y)*scale));target[2]=static_cast<std::uint8_t>(std::min(255.0,double(pixel.z)*scale));target[3]=static_cast<std::uint8_t>(exponent+128);
        }
        if(image.width<8)bytes.insert(bytes.end(),scanline.begin(),scanline.end());
        else
        {
            bytes.insert(bytes.end(),{2,2,static_cast<std::uint8_t>(image.width>>8),static_cast<std::uint8_t>(image.width&255)});
            for(size_t channel=0;channel<4;++channel)for(std::uint32_t column=0;column<image.width;)
            {
                const auto count=std::min(128u,image.width-column);bytes.push_back(static_cast<std::uint8_t>(count));
                for(std::uint32_t index=0;index<count;++index)bytes.push_back(scanline[size_t(column+index)*4+channel]);column+=count;
            }
        }
    }
    if(!asset_io::WriteAtomic(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(path));return false;}return true;
}
}
