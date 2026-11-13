#include "D3D12PipelineLibraryStorage.h"
#include <atomic>
#include <new>
#include <utility>

namespace
{
constexpr GUID storageKey{0x2c0c660b,0xa1ad,0x4126,{0xa9,0x28,0xb8,0x65,0xba,0x4c,0xcd,0xd4}};
class Storage final : public IUnknown
{
    std::atomic<ULONG> references_{1};
    std::shared_ptr<const std::vector<char>> bytes_;
public:
    explicit Storage(std::shared_ptr<const std::vector<char>> bytes):bytes_(std::move(bytes)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override
    {
        if(!output)return E_POINTER;*output=nullptr;
        if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;
        *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override
    {const auto count=--references_;if(!count)delete this;return count;}
};
}
HRESULT RetainD3D12PipelineLibraryStorage(ID3D12Object& library,std::shared_ptr<const std::vector<char>> bytes) noexcept
{
    if(!bytes||bytes->empty())return E_INVALIDARG;
    try
    {
        auto* storage=new Storage(std::move(bytes));
        const auto result=library.SetPrivateDataInterface(storageKey,storage);
        storage->Release(); // 成功后由库持有，最后一个库引用释放时才释放缓冲。
        return result;
    }
    catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
}
