#pragma once
#include "FunctionLayer/Runtime/World.h"
#include "CoreLayer/PropertyMetadata.h"
#include <span>
#include <string_view>
#include <variant>

namespace se
{
struct ComponentProperty {std::string key;PropertyValue value;};
struct ComponentDescriptor
{
    std::string_view key,labelKey;
    std::span<const PropertyDescriptor> properties;
    bool optional=true;
    ComponentTypeId type=0;
    std::uint32_t version=1;
    bool multiple=false;
    bool (*read)(const World&,ComponentHandle,std::size_t,PropertyValue&,std::string&)=nullptr;
    bool (*write)(World&,ComponentHandle,std::size_t,const PropertyValue&,std::string&)=nullptr;
    ComponentHandle (*create)(World&,EntityHandle,std::span<const PropertyValue>,bool,std::string&)=nullptr;
    bool (*migrate)(std::uint32_t,std::vector<ComponentProperty>&,std::string&)=nullptr;
};
// 注册只在启动/安全点执行，描述及字段字符串须具有进程寿命；不提供运行中卸载。
// 成功注册会使先前取得的 span/描述指针失效，使用者在操作时重新查询。
bool RegisterComponent(const ComponentDescriptor& descriptor,std::string& error);
std::span<const ComponentDescriptor> ComponentMetadata();
const ComponentDescriptor* FindComponentDescriptor(ComponentTypeId type);
const ComponentDescriptor* FindComponentDescriptor(std::string_view key);
bool ReadComponentProperties(const World& world,ComponentHandle component,std::vector<ComponentProperty>& properties,std::string& error);
ComponentHandle CreateComponentFromProperties(World& world,EntityHandle owner,ComponentTypeId type,std::uint32_t version,
    std::vector<ComponentProperty> properties,bool enabled,std::string& error);
// 所有目标对象须先创建。映射只替换内部引用，外部引用保留稳定 ID。
bool CloneComponentProperties(const World& source,EntityHandle from,World& target,EntityHandle to,std::string& error,
    const std::unordered_map<StableEntityId,StableEntityId>& references={});
const PropertyDescriptor* FindProperty(std::string_view key);
bool HasComponent(const World& world,EntityHandle entity,std::string_view component);
bool ReadProperty(const World& world,EntityHandle entity,std::string_view key,PropertyValue& value,std::string& error);
// 仅在 World 安全点编辑；通过 Set* 校验，失败不写入部分组件。
bool WriteProperty(World& world,EntityHandle entity,std::string_view key,const PropertyValue& value,std::string& error);
bool AddComponent(World& world,EntityHandle entity,std::string_view component,std::string& error);
bool RemoveComponent(World& world,EntityHandle entity,std::string_view component,std::string& error);
}
