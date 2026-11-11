#include "D3D12Buffer.h"
#include "D3D12Upload.h"
#include "D3D12Memory.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <cstring>
#include <utility>
bool AllocateD3D12Buffer(Microsoft::WRL::ComPtr<ID3D12Resource>& output,const D3D12Context& context,
    UINT64 bytes,const Localization& text)
{
    if(output||!bytes||bytes>256ull*1024*1024||!context.memory)return false;
    D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width=bytes;description.Height=description.DepthOrArraySize=description.MipLevels=1;
    description.SampleDesc={1,0};description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto created=context.memory->Create(description,D3D12_RESOURCE_STATE_COMMON,nullptr,output,"Buffer");
    return CheckGraphicsResult(created,text,created==D3D12MemoryBudgetExceeded?"D3D12.GpuMemoryBudgetExceeded":"D3D12.CreateMeshBufferFailed");
}
bool UploadD3D12Buffer(Microsoft::WRL::ComPtr<ID3D12Resource>& output,const D3D12Context& context,
    D3D12Presentation& p,std::span<const std::byte> bytes,D3D12_RESOURCE_STATES usage,const Localization& text)
{
    if(output||bytes.empty()||bytes.size()>256ull*1024*1024||!CanUploadD3D12(context,p)
        ||(usage!=D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER&&usage!=D3D12_RESOURCE_STATE_INDEX_BUFFER))
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    Microsoft::WRL::ComPtr<ID3D12Resource> candidate;
    if(!AllocateD3D12Buffer(candidate,context,bytes.size(),text))return false;
    D3D12UploadOperation operation(p,text);if(!operation)return false;
    auto& frame=p.frames[p.frameCursor];D3D12UploadSlice upload;
    if(!CheckGraphicsResult(frame.uploads.Allocate(bytes.size(),16,upload),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memcpy(upload.cpu,bytes.data(),bytes.size());frame.uploadDestinations.push_back(candidate);
    auto& states=*p.uploadStates;
    if(!CheckGraphicsResult(states.Use(*p.commandList.Get(),*candidate.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"BufferUpload"),text,"D3D12.StateTrackingFailed")
        ||!CheckGraphicsResult(states.Use(*p.commandList.Get(),*upload.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"BufferUpload"),text,"D3D12.StateTrackingFailed"))return false;
    p.commandList->CopyBufferRegion(candidate.Get(),0,upload.resource,upload.offset,bytes.size());p.uploadedBytes+=bytes.size();
    if(!CheckGraphicsResult(states.Use(*p.commandList.Get(),*candidate.Get(),usage,"BufferUploadComplete"),text,"D3D12.StateTrackingFailed"))return false;
    if(!operation.Finish())return false;
    output=std::move(candidate);return true;
}
