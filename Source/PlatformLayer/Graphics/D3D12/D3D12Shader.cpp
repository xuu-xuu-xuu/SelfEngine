#include "D3D12Shader.h"
#include "D3D12ShaderProgram.h"
#include "D3D12Result.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <string>
#include <utility>

bool CompileD3D12Shader(const std::filesystem::path& path,const char* entry,const char* target,
    Microsoft::WRL::ComPtr<ID3DBlob>& output,const Localization& localization,UINT additionalFlags)
{
    if(path.empty()||!entry||!target)
    {Log(LogLevel::Error,localization.GetText("D3D12.ShaderCompileFailed"));return false;}
    const UINT flags=D3D12ShaderCompileFlags()|additionalFlags;
    Microsoft::WRL::ComPtr<ID3DBlob> compiled,diagnostics;
    const HRESULT result=D3DCompileFromFile(path.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry,target,flags,0,compiled.GetAddressOf(),diagnostics.GetAddressOf());
    if(FAILED(result))
    {
        const auto utf8=path.u8string();
        Log(LogLevel::Error,localization.GetText("D3D12.ShaderCompileFailed")+": "
            +std::string(utf8.begin(),utf8.end())+" ["+entry+"]");
        if(diagnostics)Log(LogLevel::Error,std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),diagnostics->GetBufferSize()));
        return CheckGraphicsResult(result,localization,"D3D12.ShaderCompileFailed");
    }
    output=std::move(compiled);
    return true;
}
