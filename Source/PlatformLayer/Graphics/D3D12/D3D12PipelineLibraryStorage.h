#pragma once
#include <d3d12.h>
#include <memory>
#include <vector>

// CreatePipelineLibrary 借用输入内存；所有权绑定到实际 COM 对象，而不是缓存包装器。
// 失败时调用者必须在 bytes 失效前释放借用该内存的库。
HRESULT RetainD3D12PipelineLibraryStorage(ID3D12Object& library,
    std::shared_ptr<const std::vector<char>> bytes) noexcept;
