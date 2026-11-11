#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>

class Localization;
// 成功后替换字节码，失败保留旧值；诊断保留源码路径、行号与编译器原文。
bool CompileD3D12Shader(const std::filesystem::path& path,const char* entry,const char* target,
    Microsoft::WRL::ComPtr<ID3DBlob>& output,const Localization& localization,UINT additionalFlags=0);
