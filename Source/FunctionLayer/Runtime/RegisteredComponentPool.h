#pragma once
#include "ComponentBase.h"
#include <unordered_map>
#include <memory>
#include <span>

namespace se
{
// 只擦除池接口，池中保存实际派生类型；每种类型一个池，不切片组件。
class RegisteredComponentPool
{
public:
    virtual ~RegisteredComponentPool()=default;
    virtual ComponentBase* Get(std::uint32_t index)=0;
    virtual void Remove(std::uint32_t index)=0;
    virtual const void* TypeToken()const=0;
    virtual std::unique_ptr<RegisteredComponentPool> Empty()const=0;
    virtual std::size_t Size()const=0;
    virtual void ReserveAdditional(std::size_t count)=0;
    virtual void TransferMapped(RegisteredComponentPool& source,std::span<const std::uint32_t> indices)noexcept=0;
};
template<class T> inline const char registeredComponentTypeToken=0;
template<class T> class TypedRegisteredComponentPool final:public RegisteredComponentPool
{
public:
    ComponentBase* Get(std::uint32_t index)override
    {const auto it=values.find(index);return it==values.end()?nullptr:&it->second;}
    void Remove(std::uint32_t index)override{values.erase(index);}
    const void* TypeToken()const override{return &registeredComponentTypeToken<T>;}
    std::unique_ptr<RegisteredComponentPool> Empty()const override{return std::make_unique<TypedRegisteredComponentPool<T>>();}
    std::size_t Size()const override{return values.size();}
    void ReserveAdditional(std::size_t count)override{values.reserve(values.size()+count);}
    void TransferMapped(RegisteredComponentPool& source,std::span<const std::uint32_t> indices)noexcept override
    {
        auto& incoming=static_cast<TypedRegisteredComponentPool<T>&>(source).values;
        while(!incoming.empty()){auto node=incoming.extract(incoming.begin());node.key()=indices[node.key()];values.insert(std::move(node));}
    }
    std::unordered_map<std::uint32_t,T> values;
};
}
