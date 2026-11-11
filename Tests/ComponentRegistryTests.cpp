#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ToolLayer/Editor/EditorSession.h"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
using Json=nlohmann::json;
void Check(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
std::uint64_t nextToken=1;
class LinkComponent final:public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId=65537;
    float gain=1;
    PropertyEntityReference target;
    std::uint64_t runtimeToken=0;
    int attaches=0,enables=0;
    float valueAtAttach=0;
protected:
    void OnAttach()override{runtimeToken=nextToken++;++attaches;valueAtAttach=gain;}
    void OnEnable()override{++enables;}
};
const std::array fields{
    PropertyDescriptor{"link.gain","Property.Link.Gain",PropertyType::Number,1.0f,0.0f,10.0f},
    PropertyDescriptor{"link.target","Property.Link.Target",PropertyType::EntityReference,PropertyEntityReference{},std::nullopt,std::nullopt,true,{},PropertyPresentation::Default,true}};
bool Read(const World& world,ComponentHandle handle,std::size_t index,PropertyValue& value,std::string& error)
{
    const auto* component=world.GetRegisteredComponent<LinkComponent>(handle);
    if(!component){error="World.InvalidComponent";return false;}
    if(index==0)value=component->gain;else value=component->target;return true;
}
bool Write(World& world,ComponentHandle handle,std::size_t index,const PropertyValue& value,std::string& error)
{
    const auto* current=world.GetRegisteredComponent<LinkComponent>(handle);if(!current){error="World.InvalidComponent";return false;}
    auto candidate=*current;if(index==0)candidate.gain=std::get<float>(value);else candidate.target=std::get<PropertyEntityReference>(value);
    if(world.UpdateRegisteredComponent(handle,candidate))return true;error=world.LastError();return false;
}
ComponentHandle Create(World& world,EntityHandle owner,std::span<const PropertyValue> values,bool enabled,std::string& error)
{
    LinkComponent candidate;candidate.gain=std::get<float>(values[0]);candidate.target=std::get<PropertyEntityReference>(values[1]);
    const auto handle=world.AttachRegisteredComponent(owner,candidate,enabled);if(!handle)error=world.LastError();return handle;
}
bool Migrate(std::uint32_t version,std::vector<ComponentProperty>& properties,std::string& error)
{
    if(version!=1){error="Metadata.UnsupportedVersion";return false;}
    for(auto& property:properties)if(property.key=="strength")property.key="gain";
    return true;
}
const LinkComponent* Link(const World& world,EntityHandle owner){return world.GetRegisteredComponent<LinkComponent>(world.FindComponent(owner,LinkComponent::TypeId));}
std::string ReadFile(const std::filesystem::path& path){std::ifstream stream(path,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
}
int main(int argc,char** argv)
{
    try
    {
        std::string error;
        ComponentDescriptor descriptor{"link","Component.Link",fields,true,LinkComponent::TypeId,2,false,Read,Write,Create,Migrate};
        Check(RegisterComponent(descriptor,error),"register extension");
        Check(!RegisterComponent(descriptor,error)&&error=="Metadata.DuplicateType","duplicate ID/key rejected");
        auto bad=descriptor;bad.type=65538;bad.key="other";Check(!RegisterComponent(bad,error)&&error=="Metadata.InvalidDescriptor","bad property prefix rejected");
        World source;const auto owner=source.Create("owner",100),target=source.Create("target",200);
        Check(source.SetParent(owner,target)&&source.SetEnabled(target,false),"parent after owner and inactive ancestor");
        const auto handle=CreateComponentFromProperties(source,owner,descriptor.type,2,{{"gain",4.0f},{"target",PropertyEntityReference{200}}},false,error);
        Check(bool(handle),"registered factory");const auto* original=Link(source,owner);
        Check(original&&original->gain==4&&original->valueAtAttach==4&&original->attaches==1&&original->enables==0&&!original->Enabled(),"complete properties before lifecycle");
        const auto token=original->runtimeToken;
        Check(!AddComponent(source,owner,"link",error),"single instance enforced");
        Check(WriteProperty(source,owner,"link.gain",5.0f,error)&&Link(source,owner)->Handle()==handle,"generic property writes actual component and keeps identity");
        Check(!WriteProperty(source,owner,"link.gain",11.0f,error)&&Link(source,owner)->gain==5,"range atomic");
        Check(!WriteProperty(source,owner,"link.target",PropertyEntityReference{999},error)&&error=="Metadata.MissingReference","missing reference edit rejected");
        std::string text;Check(SerializeScene(source,text,error),"serialize extension");
        const auto json=Json::parse(text);Check(json["version"]==2&&text.find("runtimeToken")==std::string::npos&&text.find("generation")==std::string::npos,"only properties and stable identity persisted");
        World restored;Check(DeserializeScene(text,restored,error),"v2 roundtrip");
        const auto restoredOwner=restored.FindByStableId(100);const auto* restoredLink=Link(restored,restoredOwner);
        Check(restoredLink&&restoredLink->gain==5&&restoredLink->target.stableId==200&&restored.Get(restoredOwner)->parent==restored.FindByStableId(200)
            &&!restoredLink->Enabled()&&restoredLink->runtimeToken!=token&&restoredLink->attaches==1&&restoredLink->enables==0,"forward references, ancestor and own enable retained");
        World clone(source);const auto clonedOwner=clone.FindByStableId(100);const auto* cloned=Link(clone,clonedOwner);
        Check(cloned&&cloned->Handle()!=handle&&cloned->runtimeToken!=token&&cloned->gain==5&&cloned->attaches==1&&cloned->enables==0,"clone rehydrates properties and fresh lifecycle");
        Check(!clone.GetComponent(handle)&&!source.GetComponent(cloned->Handle()),"cross-world component rejects");
        Check(source.Destroy(target)&&clone.IsAlive(clonedOwner)&&Link(clone,clonedOwner)->target.stableId==200,"source destruction preserves clone and reference");
        std::string baseline;Check(SerializeScene(restored,baseline,error),"capture atomic baseline");
        auto reject=[&](Json invalid){Check(!DeserializeScene(invalid.dump(),restored,error),"bad record rejected");std::string after;Check(SerializeScene(restored,after,error)&&after==baseline,"failed load keeps active world");};
        auto invalid=json;invalid["version"]=99;reject(invalid);
        invalid=json;invalid["entities"][0]["components"][1]["type"]="999999";reject(invalid);
        invalid=json;invalid["entities"][0]["components"][1]["version"]=99;reject(invalid);
        invalid=json;invalid["entities"][0]["components"][1]["properties"]["unknown"]=1;reject(invalid);
        invalid=json;invalid["entities"][0]["components"][1]["properties"]["gain"]="wrong";reject(invalid);
        invalid=json;invalid["entities"][0]["components"][1]["properties"]["target"]="999";reject(invalid);
        invalid=json;invalid["entities"][0]["components"].push_back(invalid["entities"][0]["components"][1]);reject(invalid);
        invalid=json;invalid["entities"][0]["components"][1]["properties"].erase("gain");reject(invalid);
        invalid=json;invalid["entities"][0]["components"].erase(0);reject(invalid);
        auto previous=json;auto& oldRecord=previous["entities"][0]["components"][1];oldRecord["version"]=1;oldRecord["properties"]["strength"]=oldRecord["properties"]["gain"];oldRecord["properties"].erase("gain");
        World migrated;Check(DeserializeScene(previous.dump(),migrated,error)&&Link(migrated,migrated.FindByStableId(100))->gain==5,"registered version migration");
        const std::string legacy=R"({"format":"SelfEngine.Scene","version":1,"entities":[{"id":"1","name":"old","enabled":true,"parent":null,"transform":{"position":[1,2,3],"rotation":[0,0,0,1],"scale":[1,1,1]},"components":{"mesh":{"asset":"mesh","materials":[]},"light":{"color":[1,1,1],"intensity":2}}}]})";
        World old;Check(DeserializeScene(legacy,old,error),"v1 migrate");
        const auto oldEntity=old.FindByStableId(1);Check(old.GetMesh(oldEntity)->castsShadow&&old.GetLight(oldEntity)->type==LightType::Directional&&old.GetTransform(oldEntity)->position.y==2,"v1 defaults and properties");
        std::string upgraded;Check(SerializeScene(old,upgraded,error)&&Json::parse(upgraded)["version"]==2,"v1 next save upgrades to v2");
        EditorSession editor;Check(editor.Apply("registered scene",[&](World& world){return DeserializeScene(text,world,error);}),"editor loads registered component");
        Check(editor.SetProperty(100,"link.gain",8.0f)&&Link(editor.EditWorld(),editor.EditWorld().FindByStableId(100))->gain==8,"CPU generic property edit");
        Check(editor.Undo()&&Link(editor.EditWorld(),editor.EditWorld().FindByStableId(100))->gain==5,"extension undo");
        Check(editor.Redo()&&Link(editor.EditWorld(),editor.EditWorld().FindByStableId(100))->gain==8,"extension redo");
        const auto duplicate=editor.Duplicate(200);Check(duplicate!=0,"generic subtree duplicate");
        const LinkComponent* duplicateLink=nullptr;
        for(const auto entity:editor.EditWorld().Entities())if(editor.EditWorld().Get(entity)->parent==editor.EditWorld().FindByStableId(duplicate))duplicateLink=Link(editor.EditWorld(),entity);
        Check(duplicateLink&&duplicateLink->target.stableId==duplicate&&duplicateLink->gain==8&&!duplicateLink->Enabled(),"subtree internal stable reference remapped");
        Check(editor.StartPlay(),"play clone registered extension");editor.StopPlay();
        const std::filesystem::path proof=argc>1?argv[1]:"out/component-registry/verification.json";
        std::filesystem::create_directories(proof.parent_path());const auto scene=proof.parent_path()/"scene.json";
        Check(SaveScene(scene,restored,error),"atomic registered save");const auto file=ReadFile(scene);
        Check(restored.Destroy(restored.FindByStableId(200)),"remove registered owner subtree");
        {std::ofstream broken(proof.parent_path()/"broken.json");broken<<"{broken";}
        Check(!LoadScene(proof.parent_path()/"broken.json",restored,error)&&ReadFile(scene)==file&&restored.Size()==0,"failed file load preserves world and original file");
        std::ofstream(proof)<<Json{{"success",true},{"sceneVersion",2},{"registeredType",descriptor.type},{"duplicateRejected",true},{"actualProperties",true},{"v1Migration",true},{"componentMigration",true},{"forwardReferences",true},{"atomicFailureCases",9},{"cloneRuntimeIndependent",true},{"initialPropertiesBeforeLifecycle",true},{"editorUndoRedo",true},{"subtreeReferenceRemap",true},{"runtimeFieldsExcluded",true}}.dump(2);
        std::cout<<"ComponentRegistry passed: registered properties, migration, references, clone and editor\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
