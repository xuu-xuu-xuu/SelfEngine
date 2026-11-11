#pragma once

#include "D3D12Context.h"
#include "D3D12Presentation.h"
#include "D3D12Result.h"
#include "D3D12StateTracker.h"

inline bool CanUploadD3D12(const D3D12Context& context,const D3D12Presentation& p)
{
    if(!context.device||!context.states||context.states!=p.states||!p.queue||!p.commandList||!p.frames[0].allocator||!p.fence||!p.fenceEvent.IsValid()
        ||!p.swapChain||p.failed)return false;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    return SUCCEEDED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))&&device.Get()==context.device.Get();
}
// 显式批次内只追加；独立调用保留同步完成语义。失败退出也收拢已记录的成功拷贝。
class D3D12UploadOperation
{
public:
    D3D12UploadOperation(D3D12Presentation& value,const Localization& text):p_(value),text_(text),owned_(!value.uploadStates)
    {valid_=!owned_||BeginD3D12UploadBatch(p_,text_);}
    ~D3D12UploadOperation(){if(owned_&&valid_&&!finished_&&!Finish())ReleaseD3D12Presentation(p_);}
    explicit operator bool() const{return valid_;}
    bool Finish()
    {
        if(finished_)return valid_;finished_=true;
        if(!valid_||!owned_)return valid_;
        UINT64 fence=0;
        valid_=EndD3D12UploadBatch(p_,fence,text_)&&WaitD3D12Submission(p_,fence,text_,D3D12WaitReason::Upload);
        CollectD3D12UploadReferences(p_);return valid_;
    }
private:
    D3D12Presentation& p_;const Localization& text_;bool owned_,valid_=false,finished_=false;
};
