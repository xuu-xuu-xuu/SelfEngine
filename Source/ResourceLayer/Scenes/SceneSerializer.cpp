#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "ResourceLayer/PropertyJson.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <unordered_set>
#include <charconv>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <Windows.h>

namespace se
{
struct SceneDocument::Data {nlohmann::json root;};
namespace
{
using Json=nlohmann::json;
constexpr std::size_t maxSceneBytes=16*1024*1024;
constexpr std::size_t maxEntities=100000;
constexpr std::size_t maxRetiredIds=100000;
std::atomic<std::uint64_t> temporarySequence{1};

bool ReadFloat(const Json& json,float& value)
{
    if(!json.is_number())return false;
    const double number=json.get<double>();
    if(!std::isfinite(number) || number>(std::numeric_limits<float>::max)() || number< -(std::numeric_limits<float>::max)())return false;
    value=static_cast<float>(number);return true;
}
bool ReadId(const Json& json,StableEntityId& id)
{
    if(!json.is_string())return false;
    const auto& text=json.get_ref<const std::string&>();
    const auto result=std::from_chars(text.data(),text.data()+text.size(),id);
    return result.ec==std::errc{} && result.ptr==text.data()+text.size() && id!=0;
}

bool OnlyKeys(const Json& object,std::initializer_list<std::string_view> keys)
{
    if(!object.is_object())return false;
    for(auto it=object.begin();it!=object.end();++it)
        if(std::find(keys.begin(),keys.end(),it.key())==keys.end())return false;
    return true;
}
bool ReadValue(const Json& json,const PropertyDescriptor* field,PropertyValue& value)
{
    if(field)return ReadPropertyJson(json,*field,value);
    // 显式旧版本迁移可重命名标量字段；未知当前字段由注册入口拒绝。
    if(json.is_boolean()){value=json.get<bool>();return true;}
    if(json.is_string()){value=json.get<std::string>();return true;}
    float number=0;if(ReadFloat(json,number)){value=number;return true;}return false;
}
bool MigrateV1(Json& root,std::string& error)
{
    for(auto& entity:root["entities"])
    {
        if(!OnlyKeys(entity,{"id","name","enabled","parent","transform","components"})||!entity.contains("transform")||!entity.contains("components")||!entity["components"].is_object())
        {error="Scene.InvalidData";return false;}
        Json records=Json::array();
        records.push_back({{"type",std::to_string(ComponentTypes::Transform)},{"version",1},{"enabled",true},{"properties",entity["transform"]}});
        for(auto it=entity["components"].begin();it!=entity["components"].end();++it)
        {
            const auto* descriptor=FindComponentDescriptor(it.key());
            if(!descriptor||descriptor->type==ComponentTypes::Transform||descriptor->type>=1024){error="Scene.UnknownComponent";return false;}
            Json properties=it.value();if(!properties.is_object()){error="Scene.InvalidData";return false;}
            if(descriptor->type==ComponentTypes::Mesh)
            {if(!properties.contains("castsShadow"))properties["castsShadow"]=true;if(!properties.contains("receivesShadow"))properties["receivesShadow"]=true;}
            if(descriptor->type==ComponentTypes::Light)
            {
                if(!properties.contains("type"))properties["type"]="directional";
                if(!properties.contains("range"))properties["range"]=10.0f;
                if(properties["type"]!="point"&&properties["type"]!="directional"){error="Scene.InvalidData";return false;}
                properties["type"]=properties["type"]=="point";
            }
            records.push_back({{"type",std::to_string(descriptor->type)},{"version",descriptor->version},{"enabled",true},{"properties",std::move(properties)}});
        }
        entity.erase("transform");entity["components"]=std::move(records);
    }
    root["version"]=2;return true;
}
}

bool SerializeScene(const World& world,std::string& jsonText,std::string& error)
{
    error.clear();
    if(world.IsIterating()||world.HasPendingCommands()){error="Scene.PendingCommands";return false;}
    try
    {
        if(world.Size()>maxEntities){error="Scene.TooLarge";return false;}
        Json root{{"format","SelfEngine.Scene"},{"version",2},{"entities",Json::array()},{"retiredIds",Json::array()}};
        const auto& environment=world.EnvironmentSettings();
        if(!environment.todProfile.empty()||environment.preserveTime)root["environment"]={{"todProfile",environment.todProfile},{"preserveTime",environment.preserveTime}};
        for(const auto id:world.KnownStableIds())if(!world.FindByStableId(id))root["retiredIds"].push_back(std::to_string(id));
        if(root["retiredIds"].size()>maxRetiredIds){error="Scene.TooLarge";return false;}
        // 持久输出按稳定 ID 排序，不能让场景提交复用运行槽位改变 Dirty/历史判断。
        auto entities=world.Entities();
        std::sort(entities.begin(),entities.end(),[&](EntityHandle a,EntityHandle b){return world.Get(a)->stableId<world.Get(b)->stableId;});
        for(const auto entity:entities)
        {
            const auto& record=*world.Get(entity);Json components=Json::array();
            for(const auto handle:world.Components(entity))
            {
                const auto* component=world.GetComponent(handle);if(component->Type()==ComponentTypes::Script)continue;
                const auto* descriptor=FindComponentDescriptor(component->Type());if(!descriptor){error="Scene.UnknownComponent";return false;}
                std::vector<ComponentProperty> fields;if(!ReadComponentProperties(world,handle,fields,error))return false;
                Json properties=Json::object();for(const auto& field:fields)properties[field.key]=WritePropertyJson(field.value);
                components.push_back({{"type",std::to_string(descriptor->type)},{"version",descriptor->version},{"enabled",component->Enabled()},{"properties",std::move(properties)}});
            }
            root["entities"].push_back({{"id",std::to_string(record.stableId)},{"name",record.name},{"enabled",record.enabled},
                {"parent",record.parent?Json(std::to_string(world.Get(record.parent)->stableId)):Json(nullptr)},{"components",std::move(components)}});
        }
        auto result=root.dump(2);if(result.size()>maxSceneBytes){error="Scene.TooLarge";return false;}
        jsonText=std::move(result);return true;
    }
    catch(const Json::exception&){error="Scene.InvalidData";return false;}
}

bool ParseSceneDocument(std::string_view jsonText,SceneDocument& document,std::string& error)
{
    error.clear();
    if(jsonText.size()>maxSceneBytes){error="Scene.TooLarge";return false;}
    try
    {
        bool duplicate=false;std::vector<std::unordered_map<std::string,bool>> objectKeys;
        Json root=Json::parse(jsonText.begin(),jsonText.end(),[&](int depth,Json::parse_event_t event,Json& parsed){
            if(depth>64)throw std::runtime_error("Scene.TooDeep");
            if(event==Json::parse_event_t::object_start)objectKeys.emplace_back();
            else if(event==Json::parse_event_t::key&&!objectKeys.empty()){if(!objectKeys.back().emplace(parsed.get<std::string>(),true).second)duplicate=true;}
            else if(event==Json::parse_event_t::object_end&&!objectKeys.empty())objectKeys.pop_back();return true;
        });
        if(duplicate||!OnlyKeys(root,{"format","version","entities","retiredIds","environment"})||root.value("format",std::string{})!="SelfEngine.Scene"
            ||!root.contains("version")||!root["version"].is_number_integer()){error="Scene.InvalidData";return false;}
        if(root["version"]!=1&&root["version"]!=2){error="Scene.UnsupportedVersion";return false;}
        if(!root.contains("entities")||!root["entities"].is_array()||root["entities"].size()>maxEntities){error="Scene.InvalidData";return false;}
        document.root_=std::make_shared<const SceneDocument::Data>(SceneDocument::Data{std::move(root)});return true;
    }
    catch(const Json::exception&){error="Scene.InvalidJson";return false;}
    catch(const std::runtime_error& exception)
    {if(std::string_view(exception.what())=="Scene.TooDeep"){error="Scene.TooDeep";return false;}throw;}
}
bool InstantiateScene(const SceneDocument& document,World& world,std::string& error)
{
    error.clear();
    if(!document.root_){error="Scene.InvalidData";return false;}
    if(world.IsSceneCommitBoundary()||world.IsInFrame()||world.IsIterating()||world.HasPendingCommands()){error="Scene.PendingCommands";return false;}
    try
    {
        Json root=document.root_->root;
        if(root["version"]==1&&!MigrateV1(root,error))return false;
        const bool publish=!world.IsStaging();World loaded{World::StagingTag{}};std::vector<std::pair<StableEntityId,StableEntityId>> parents;
        loaded.externalReference_=world.externalReference_;
        if(root.contains("environment"))
        {
            const auto& settings=root["environment"];
            if(!OnlyKeys(settings,{"todProfile","preserveTime"})||!settings.contains("todProfile")||!settings["todProfile"].is_string()
                ||!settings.contains("preserveTime")||!settings["preserveTime"].is_boolean()
                ||!loaded.SetEnvironmentSettings({settings["todProfile"].get<std::string>(),settings["preserveTime"].get<bool>()}))
            {error="TOD.InvalidProfileId";return false;}
        }
        for(const auto& data:root["entities"])
        {
            StableEntityId id=0,parent=0;
            if(!OnlyKeys(data,{"id","name","enabled","parent","components"})||!data.contains("id")||!ReadId(data["id"],id)
                ||!data.contains("name")||!data["name"].is_string()||!data.contains("enabled")||!data["enabled"].is_boolean()
                ||!data.contains("parent")||(!data["parent"].is_null()&&!ReadId(data["parent"],parent))
                ||!data.contains("components")||!data["components"].is_array()||data["components"].size()>4096){error="Scene.InvalidData";return false;}
            const auto entity=loaded.Create(data["name"].get<std::string>(),id);
            if(!entity||!loaded.SetEnabled(entity,data["enabled"].get<bool>())){error=loaded.LastError();return false;}
            parents.emplace_back(id,parent);
        }
        for(const auto [id,parent]:parents)
        {
            if(!parent)continue;const auto parentEntity=loaded.FindByStableId(parent);
            if(!parentEntity){error="Scene.MissingParent";return false;}
            if(!loaded.SetParent(loaded.FindByStableId(id),parentEntity)){error=loaded.LastError();return false;}
        }
        if(root.contains("retiredIds"))
        {
            const auto& ids=root["retiredIds"];std::unordered_set<StableEntityId> unique;
            if(!ids.is_array()||ids.size()>maxRetiredIds){error="Scene.InvalidData";return false;}
            for(const auto& item:ids){StableEntityId id=0;if(!ReadId(item,id)||loaded.FindByStableId(id)||!unique.insert(id).second||!loaded.ReserveStableId(id)){error="Scene.InvalidData";return false;}}
        }
        for(const auto& data:root["entities"])
        {
            StableEntityId id=0;ReadId(data["id"],id);const auto entity=loaded.FindByStableId(id);bool transform=false;
            for(const auto& record:data["components"])
            {
                ComponentTypeId type=0;
                if(!OnlyKeys(record,{"type","version","enabled","properties"})||!record.contains("type")||!ReadId(record["type"],type)
                    ||!record.contains("version")||!record["version"].is_number_integer()||record["version"].get<std::int64_t>()<1||record["version"].get<std::uint64_t>()>UINT32_MAX
                    ||!record.contains("enabled")||!record["enabled"].is_boolean()||!record.contains("properties")||!record["properties"].is_object())
                {error="Scene.InvalidData";return false;}
                const auto* descriptor=FindComponentDescriptor(type);if(!descriptor){error="Scene.UnknownComponent";return false;}
                if(type==ComponentTypes::Transform){if(transform){error="Scene.InvalidData";return false;}transform=true;}
                std::vector<ComponentProperty> fields;
                for(auto it=record["properties"].begin();it!=record["properties"].end();++it)
                {
                    const auto field=std::find_if(descriptor->properties.begin(),descriptor->properties.end(),[&](const auto& p){return SerializedPropertyKey(p)==it.key();});
                    PropertyValue value;if(!ReadValue(it.value(),field==descriptor->properties.end()?nullptr:&*field,value)){error="Metadata.InvalidValue";return false;}
                    fields.push_back({it.key(),std::move(value)});
                }
                if(!CreateComponentFromProperties(loaded,entity,type,record["version"].get<std::uint32_t>(),std::move(fields),record["enabled"].get<bool>(),error))return false;
            }
            if(!transform){error="Scene.MissingTransform";return false;}
        }
        world=std::move(loaded);if(publish)world.ActivateStagedComponents();return true;
    }
    catch(const Json::exception&){error="Scene.InvalidJson";return false;}
    catch(const std::runtime_error& exception)
    {if(std::string_view(exception.what())=="Scene.TooDeep"){error="Scene.TooDeep";return false;}throw;}
}

bool DeserializeScene(std::string_view text,World& world,std::string& error)
{
    SceneDocument document;return ParseSceneDocument(text,document,error)&&InstantiateScene(document,world,error);
}

bool SaveScene(const std::filesystem::path& path,const World& world,std::string& error,bool replaceExisting)
{
    std::string text;
    if(!SerializeScene(world,text,error))return false;
    if(path.empty() || path.filename().empty()) {error="Scene.InvalidPath";return false;}
    const auto temporary=path.parent_path()/(path.filename().wstring()+L".tmp."
        +std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(temporarySequence.fetch_add(1)));
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) {error="Scene.WriteFailed";return false;}
    DWORD written=0;
    const bool stored=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)
        && written==text.size() && FlushFileBuffers(file);
    const bool closed=CloseHandle(file)!=FALSE;
    if(!stored || !closed) {DeleteFileW(temporary.c_str());error="Scene.WriteFailed";return false;}
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH|(replaceExisting?MOVEFILE_REPLACE_EXISTING:0)))
    {DeleteFileW(temporary.c_str());error="Scene.ReplaceFailed";return false;}
    error.clear();return true;
}

bool ReadSceneDocument(const std::filesystem::path& path,SceneDocument& document,std::string& error)
{
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input) {error="Scene.ReadFailed";return false;}
    const auto size=input.tellg();
    if(size<0 || static_cast<std::uint64_t>(size)>maxSceneBytes) {error="Scene.TooLarge";return false;}
    std::string text(static_cast<std::size_t>(size),'\0');input.seekg(0);
    if(!text.empty() && !input.read(text.data(),static_cast<std::streamsize>(text.size())))
    {error="Scene.ReadFailed";return false;}
    return ParseSceneDocument(text,document,error);
}
bool LoadScene(const std::filesystem::path& path,World& world,std::string& error)
{SceneDocument document;return ReadSceneDocument(path,document,error)&&InstantiateScene(document,world,error);}
}
