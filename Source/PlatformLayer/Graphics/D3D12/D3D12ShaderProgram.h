#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <d3dcompiler.h>
#include <wrl/client.h>

class Localization;
struct D3D12ShaderDependency {std::string path,hash;};
struct D3D12ShaderSource
{
    std::filesystem::path path;
    se::rhi::PipelineKind kind=se::rhi::PipelineKind::TexturedMesh;
    UINT flags=0;
    UINT layoutVersion=se::rhi::MaterialLayoutVersion;
    bool compressedVertex=false;
    std::vector<se::rhi::ShaderMacro> macros;
    std::vector<D3D12ShaderDependency> dependencies;
    std::string key,preprocessed;
};
struct D3D12ShaderProgram
{
    D3D12ShaderSource source;
    Microsoft::WRL::ComPtr<ID3DBlob> vertex,pixel;
    std::string bytecodeKey;
};
struct D3D12ShaderLoadStatistics {UINT64 artifactLoads=0,artifactRejections=0,compilations=0;};
UINT D3D12ShaderCompileFlags();
bool ReadD3D12ShaderSource(const se::rhi::PipelineDescription& description,D3D12ShaderSource& output,const Localization& text);
std::shared_ptr<const D3D12ShaderProgram> CompileD3D12ShaderProgram(D3D12ShaderSource source,const Localization& text);
std::shared_ptr<const D3D12ShaderProgram> LoadD3D12ShaderProgram(D3D12ShaderSource source,D3D12ShaderLoadStatistics& stats,const Localization& text);
bool WriteD3D12ShaderProgram(const D3D12ShaderProgram& program,const std::filesystem::path& artifact,const Localization& text);
// 固定PBR接口包：单文件原子发布，保留反射，去除源码调试/私有/内嵌Root Signature块。
std::string D3D12UserPbrInterfaceDescription();
std::string D3D12UserPbrInterfaceHash();
bool WriteD3D12UserPbrPackage(const D3D12ShaderProgram& program,const std::filesystem::path& artifact,const Localization& text);
std::shared_ptr<const D3D12ShaderProgram> LoadD3D12UserPbrPackage(const se::rhi::PipelineDescription& description,
    D3D12ShaderLoadStatistics& stats,const Localization& text);
