#pragma once
#include "ResourceLayer/Assets/AssetStore.h"

namespace se
{
struct LoadingWorkload
{
    std::filesystem::path root;
    std::uint32_t width=1025,height=513,seed=424242;
    std::vector<AssetId> images;
    std::string digest;
    bool light=false;
};
bool CreateLoadingWorkload(const std::filesystem::path& directory,bool light,std::vector<AssetDiagnostic>& diagnostics);
bool ReadLoadingWorkload(const std::filesystem::path& directory,LoadingWorkload& output,std::vector<AssetDiagnostic>& diagnostics);
}
