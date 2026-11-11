#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ResourceLayer/Assets/MaterialMetadata.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/PropertyJson.h"
#include "ToolLayer/Editor/EditorSession.h"
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
using namespace se;
void Check(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
int main(int argc,char** argv)
{
    try
    {
        World world;const auto entity=world.Create("metadata",42);std::string error;PropertyValue value;
        std::set<std::string_view> keys;
        for(const auto& component:ComponentMetadata())
        {
            if(component.optional)Check(AddComponent(world,entity,component.key,error),"add described component");
            for(const auto& field:component.properties)
            {
                Check(keys.insert(field.key).second,"unique property key");
                Check(ReadProperty(world,entity,field.key,value,error),"read default");
                Check(value.index()==field.defaultValue.index(),"type matches descriptor");
                Check(WritePropertyJson(value)==WritePropertyJson(field.defaultValue),"owner default agrees with metadata");
                Check(!SerializedPropertyKey(field).empty(),"stable serialization key");
                PropertyValue decoded;Check(ReadPropertyJson(WritePropertyJson(value),field,decoded)&&WritePropertyJson(decoded)==WritePropertyJson(value),"shared value codec roundtrip");
            }
        }
        Check(WriteProperty(world,entity,"transform.position",Vec3{1,2,3},error),"edit position");
        Check(WriteProperty(world,entity,"transform.rotation",Quat{0,0,0,2},error),"normalize rotation");
        Check(world.GetTransform(entity)->rotation.w==1,"rotation normalized through world");
        Check(WriteProperty(world,entity,"transform.scale",Vec3{0,1,1},error)&&world.GetTransform(entity)->scale.x==0,"scale follows world policy including singular transforms");
        Check(!WriteProperty(world,entity,"light.intensity",std::string("wrong"),error),"type rejected");
        Check(!WriteProperty(world,entity,"light.intensity",-1.0f,error),"range rejected");
        Check(!WriteProperty(world,entity,"camera.near",2000.0f,error)&&world.GetCamera(entity)->nearPlane==0.1f,"cross field validation atomic");
        Check(WriteProperty(world,entity,"mesh.asset",std::string("mesh-id"),error),"asset field");
        Check(WriteProperty(world,entity,"mesh.materials",std::vector<std::string>{"material-a"},error),"asset list");
        world.ForEach([&](EntityHandle handle){Check(!WriteProperty(world,handle,"light.intensity",3.0f,error),"iteration edit rejected");});
        Check(!RemoveComponent(world,entity,"transform",error),"transform required");
        std::string saved;Check(SerializeScene(world,saved,error),"save metadata changes");World restored;
        Check(DeserializeScene(saved,restored,error),"restore metadata changes");
        Check(ReadProperty(restored,restored.FindByStableId(42),"transform.position",value,error)&&std::get<Vec3>(value).y==2,"edited data survives scene roundtrip");
        Check(RemoveComponent(world,entity,"camera",error)&&!ReadProperty(world,entity,"camera.fov",value,error),"missing component read rejected");
        world.Destroy(entity);Check(!WriteProperty(world,entity,"transform.position",Vec3{},error),"stale entity rejected");
        EditorSession editor;const auto id=editor.Create("property editor");const auto before=editor.UndoCount();
        Check(editor.BeginEdit("Property.Transform.Position"),"start metadata drag transaction");
        for(int index=1;index<=10;++index)Check(editor.SetProperty(id,"transform.position",Vec3{float(index),0,0}),"session typed property write");
        Check(editor.CommitEdit()&&editor.UndoCount()==before+1,"ten property changes merge to one command");
        Check(editor.Undo()&&editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(id))->position.x==0,"property undo");
        Check(editor.Redo()&&editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(id))->position.x==10,"property redo");
        Check(!editor.SetProperty(id,"transform.position",false)&&editor.LastError()=="Metadata.InvalidValue","wrong type keeps precise property diagnostic");
        Check(!editor.SetProperty(id,"transform.position",Vec3{std::numeric_limits<float>::quiet_NaN(),0,0}),"nonfinite property rejected");
        Check(editor.BeginEdit("Property.Transform.Position")&&editor.SetProperty(id,"transform.position",Vec3{50,0,0})&&editor.CancelEdit(),"cancel typed property transaction");
        Check(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(id))->position.x==10,"rejected/cancelled edits preserve owner");
        Check(editor.StartPlay()&&!editor.SetProperty(id,"transform.position",Vec3{})&&editor.LastError()=="Editor.EditWhilePlaying","Play cannot mutate edit properties");editor.StopPlay();

        MaterialDocument material;keys.clear();
        for(const auto& field:MaterialProperties())
        {
            Check(keys.insert(field.key).second,"unique material property key");
            Check(ReadMaterialProperty(material,field.key,value,error)&&WritePropertyJson(value)==WritePropertyJson(field.defaultValue),"material owner default matches descriptor");
            PropertyValue decoded;Check(ReadPropertyJson(WritePropertyJson(value),field,decoded),"material property codec");
        }
        Check(WriteMaterialProperty(material,"material.name",std::string("材质 metadata"),error),"UTF8 material name");
        Check(WriteMaterialProperty(material,"material.baseColor",Vec4{.2f,.4f,.6f,.8f},error),"color vector property");
        Check(WriteMaterialProperty(material,"material.metallic",.65f,error)&&WriteMaterialProperty(material,"material.roughness",.25f,error),"bounded PBR properties");
        Check(WriteMaterialProperty(material,"material.normalScale",-2.0f,error),"normal source strength policy preserved");
        std::string materialJson;std::vector<AssetDiagnostic> diagnostics;
        Check(SerializeMaterialDocument(material,materialJson,diagnostics),"metadata material serialization");
        Check(!WriteMaterialProperty(material,"material.metallic",1.01f,error)&&!WriteMaterialProperty(material,"material.roughness",true,error),"material range/type rejected");
        Check(!WriteMaterialProperty(material,"material.baseColor",Vec4{1,1,1,std::numeric_limits<float>::infinity()},error),"nonfinite color rejected");
        Check(!WriteMaterialProperty(material,"material.baseColorTexture",std::string("invalid asset"),error),"owner validates asset ID");
        std::string unchanged;Check(SerializeMaterialDocument(material,unchanged,diagnostics)&&unchanged==materialJson,"failed material writes leave all fields unchanged");
        const auto directory=std::filesystem::path(argc>1?argv[1]:"out/validation/metadata")/asset_io::NewId();std::filesystem::create_directories(directory);
        const auto path=directory/"edited.material.json";Check(SaveMaterialDocument(path,material,diagnostics),"save edited material");
        MaterialDocument restoredMaterial;Check(LoadMaterialDocument(path,restoredMaterial,diagnostics),"reload edited material");
        Check(SerializeMaterialDocument(restoredMaterial,unchanged,diagnostics)&&unchanged==materialJson,"every material property survives save/reload");
        auto invalid=material;invalid.roughness=-1;Check(!SaveMaterialDocument(path,invalid,diagnostics),"invalid save rejected");
        Check(LoadMaterialDocument(path,restoredMaterial,diagnostics)&&restoredMaterial.roughness==.25f,"failed save preserves prior usable file");
        auto legacy=nlohmann::json::parse(materialJson);legacy.erase("normalScale");
        Check(DeserializeMaterialDocument(legacy.dump(),restoredMaterial,diagnostics)&&restoredMaterial.normalScale==1,"optional old material field defaults");
        legacy["roughness"]=false;const auto previous=restoredMaterial.name;
        Check(!DeserializeMaterialDocument(legacy.dump(),restoredMaterial,diagnostics)&&restoredMaterial.name==previous&&restoredMaterial.normalScale==1,"bad document does not partially replace output");
        PropertyValue untouched=23.0f;Check(!ReadPropertyJson("bad",*FindMaterialProperty("material.roughness"),untouched)&&std::get<float>(untouched)==23,"bad JSON value preserves output");
        std::cout<<"MetadataTests passed (shared descriptors/codecs, typed editor transactions, material persistence and atomic failures)\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"MetadataTests failed: "<<e.what()<<'\n';return 1;}
}
