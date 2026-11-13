#include "PlatformLayer/Graphics/D3D12/D3D12PipelineLibraryStorage.h"
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool value,const char* detail){if(!value)throw std::runtime_error(detail);}
struct Observation{bool destructorSawBytes=false;std::weak_ptr<const std::vector<char>> bytes;};
// 只模拟 D3D12Object 的 COM 私有数据契约，不创建设备或调用图形驱动。
class Library final : public ID3D12Object
{
    std::atomic<ULONG> refs_{1};Microsoft::WRL::ComPtr<IUnknown> storage_;Observation& state_;bool fail_;
public:
    Library(Observation& state,bool fail=false):state_(state),fail_(fail){}
    ~Library(){state_.destructorSawBytes=!state_.bytes.expired();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override
    {if(!output)return E_POINTER;*output=nullptr;if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(ID3D12Object))return E_NOINTERFACE;*output=static_cast<ID3D12Object*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override{auto count=--refs_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown* value) override
    {if(fail_)return E_OUTOFMEMORY;storage_=const_cast<IUnknown*>(value);return S_OK;}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override{return S_OK;}
};
}
int main()
{
    try
    {
        Observation state;
        Microsoft::WRL::ComPtr<ID3D12Object> external;
        {
            auto bytes=std::make_shared<const std::vector<char>>(std::initializer_list<char>{'P','S','O'});state.bytes=bytes;
            Microsoft::WRL::ComPtr<ID3D12Object> library;library.Attach(new Library(state));
            Check(SUCCEEDED(RetainD3D12PipelineLibraryStorage(*library.Get(),bytes)),"attach storage");
            external=library;
        }
        Check(!state.bytes.expired(),"external COM reference must retain the input blob after cache wrapper dies");
        Check(state.bytes.lock()->at(0)=='P',"retained input remains readable");
        external.Reset();
        Check(state.destructorSawBytes,"library destructor must run before blob destruction");
        Check(state.bytes.expired(),"final COM release must release blob without a cycle");
        Observation failure;
        {
            auto bytes=std::make_shared<const std::vector<char>>(1,'X');failure.bytes=bytes;
            Microsoft::WRL::ComPtr<ID3D12Object> library;library.Attach(new Library(failure,true));
            Check(FAILED(RetainD3D12PipelineLibraryStorage(*library.Get(),bytes)),"attachment failure reported");
            library.Reset();Check(failure.destructorSawBytes,"failed attachment releases library while caller still owns bytes");
        }
        Check(failure.bytes.expired(),"failed attachment leaks no owner");
        std::cout<<"Pipeline library storage passed: external references, destructor ordering, attachment failure; no GPU.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
