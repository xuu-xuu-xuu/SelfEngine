#pragma once
#include <cstdint>

namespace se
{
struct EntityHandle
{
    std::uint32_t index=UINT32_MAX,generation=0;
    constexpr std::uint64_t Pack()const{return (std::uint64_t{generation}<<32)|index;}
    static constexpr EntityHandle FromPacked(std::uint64_t value){return {static_cast<std::uint32_t>(value),static_cast<std::uint32_t>(value>>32)};}
    explicit constexpr operator bool()const{return index!=UINT32_MAX&&generation!=0;}
    bool operator==(const EntityHandle&)const=default;
};
struct ComponentHandle
{
    std::uint32_t index=UINT32_MAX,generation=0;
    constexpr std::uint64_t Pack()const{return (std::uint64_t{generation}<<32)|index;}
    static constexpr ComponentHandle FromPacked(std::uint64_t value){return {static_cast<std::uint32_t>(value),static_cast<std::uint32_t>(value>>32)};}
    explicit constexpr operator bool()const{return index!=UINT32_MAX&&generation!=0;}
    bool operator==(const ComponentHandle&)const=default;
};
using StableEntityId=std::uint64_t;
using ComponentTypeId=std::uint64_t;
namespace ComponentTypes
{
inline constexpr ComponentTypeId Transform=1,Camera=2,Mesh=3,Light=4,Script=5;
}
}
