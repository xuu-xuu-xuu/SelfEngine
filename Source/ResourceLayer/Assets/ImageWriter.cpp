#include "ImageWriter.h"
#include "AssetIO.h"
namespace se
{
bool WritePpmImage(const std::filesystem::path& path,std::uint32_t width,std::uint32_t height,std::span<const std::uint8_t> rgba8,std::vector<AssetDiagnostic>& diagnostics)
{
    const std::uint64_t pixels=std::uint64_t(width)*height;
    if(!width||!height||width>16384||height>16384||pixels>(asset_io::MaxFileBytes-64)/3||rgba8.size()!=pixels*4)
    {asset_io::Diagnostic(diagnostics,"Assets.ImageWriteFailed",asset_io::Utf8(path));return false;}
    const auto header="P6\n"+std::to_string(width)+" "+std::to_string(height)+"\n255\n";std::vector<std::uint8_t> bytes(header.begin(),header.end());bytes.reserve(header.size()+static_cast<size_t>(pixels)*3);
    for(size_t index=0;index<rgba8.size();index+=4)bytes.insert(bytes.end(),rgba8.begin()+index,rgba8.begin()+index+3);
    if(!asset_io::WriteAtomic(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ImageWriteFailed",asset_io::Utf8(path));return false;}return true;
}
}
