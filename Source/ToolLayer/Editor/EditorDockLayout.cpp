#include "EditorDockLayout.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <fstream>
#include <iterator>

namespace se
{
namespace
{
constexpr std::uintmax_t MaximumDockBytes=EditorDockLayout::MaximumBytes;
}
bool EditorDockLayout::Validate(std::string_view text,std::string& errorKey)
{
    errorKey.clear();
    if(text.empty()||text.size()>MaximumDockBytes){errorKey="Editor.DockLayoutInvalid";return false;}
    if(text.find('\0')!=std::string_view::npos){errorKey="Editor.DockLayoutInvalid";return false;}
    if(text.find(DockingSection)==std::string_view::npos){errorKey="Editor.DockLayoutInvalid";return false;}
    return true;
}
bool EditorDockLayout::Load(const std::filesystem::path& path,std::string& text,std::string& errorKey)
{
    text.clear();errorKey.clear();
    std::error_code error;
    if(!std::filesystem::exists(path,error))
    {
        if(error){errorKey="Editor.DockLayoutInvalid";return false;}
        return true;
    }
    const auto size=std::filesystem::file_size(path,error);
    if(error||size>MaximumDockBytes){errorKey="Editor.DockLayoutInvalid";return false;}
    std::ifstream input(path,std::ios::binary);
    if(!input){errorKey="Editor.DockLayoutInvalid";return false;}
    text.assign(std::istreambuf_iterator<char>(input),{});
    if(input.bad()||text.size()!=size){errorKey="Editor.DockLayoutInvalid";return false;}
    return Validate(text,errorKey);
}
std::filesystem::path EditorDockLayout::BackupPath(std::filesystem::path path)
{path+=".bak";return path;}
bool EditorDockLayout::Save(const std::filesystem::path& path,std::string_view text,std::string& errorKey)
{
    errorKey.clear();
    if(!Validate(text,errorKey))return false;
    std::error_code error;
    if(std::filesystem::exists(path,error))
    {
        std::vector<std::uint8_t> previous;
        if(!asset_io::Read(path,previous)||!asset_io::WriteAtomic(BackupPath(path),previous))
        {errorKey="Editor.DockLayoutSaveFailed";return false;}
    }
    if(!asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()}))
    {errorKey="Editor.DockLayoutSaveFailed";return false;}
    return true;
}
}
