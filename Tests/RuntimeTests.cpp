#include "CoreLayer/Math/Math.h"
#include "FunctionLayer/Runtime/World.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
bool Near(float a,float b) {return std::abs(a-b)<1e-4f;}
bool Near(se::Vec3 a,se::Vec3 b) {return Near(a.x,b.x)&&Near(a.y,b.y)&&Near(a.z,b.z);}
std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
}

int main(int argc,char* argv[])
{
    try
    {
        const std::filesystem::path output=argc>1?argv[1]:".cache/validation/runtime";
        std::filesystem::create_directories(output);
        const auto rotation=se::AxisAngle({0,1,0},se::Pi/2);
        Require(Near(se::Rotate(rotation,{1,0,0}),{0,0,-1}),"Right-handed quaternion rotation");
        se::Transform transform{{2,3,4},rotation,{2,3,4}};
        const auto matrix=transform.Matrix();
        Require(Near(se::TransformPoint(matrix,{1,0,0}),{2,3,2}),"TRS order");
        const auto inverse=se::Inverse(matrix);
        Require(inverse.has_value()&&Near(se::TransformPoint(*inverse,{2,3,2}),{1,0,0}),"Inverse transform");
        Require(!se::Inverse(se::Scaling({1,0,1})),"Singular matrix rejected");
        const auto normal=se::TransformNormal(se::Scaling({2,1,1}),se::Normalize(se::Vec3{1,1,0}));
        Require(normal&&Near(*normal,se::Normalize(se::Vec3{0.5f,1,0})),"Inverse transpose normal");

        se::World world;
        const auto old=world.Create("old");Require(world.Destroy(old),"Destroy old entity");
        const auto replacement=world.Create("replacement");
        Require(old.index==replacement.index && old.generation!=replacement.generation
            && !world.IsAlive(old) && world.GetTransform(old)==nullptr,"Stale generation rejected");
        Require(!world.SetTransform(old,{}),"Stale write rejected");
        se::World identities;const auto probe=identities.Create("probe");const auto deletedId=identities.Get(probe)->stableId+1;
        const auto explicitEntity=identities.Create("explicit identity",deletedId);
        Require(explicitEntity&&identities.Destroy(explicitEntity),"retire explicit stable identity");
        const auto fresh=identities.Create("fresh automatic identity");
        Require(fresh&&identities.Get(fresh)->stableId!=deletedId&&!identities.IsAlive(explicitEntity),"automatic ID skips deleted explicit identity and old handle");
        Require(!identities.ResolveReference(deletedId)&&identities.LastError()=="World.UnresolvedReference","missing persistent reference diagnostic");
        Require(!identities.Create("accidental resurrection",deletedId)&&identities.LastError()=="World.RetiredStableId","explicit identity reuse rejected");
        Require(identities.ResolveReference(identities.Get(fresh)->stableId)==fresh&&identities.LastError().empty(),"live stable reference resolves current generation");
        se::World identityCopy=identities;Require(!identityCopy.Create("copy reuse",deletedId),"deep copy preserves retired identity");
        std::string identityJson,identityError;Require(se::SerializeScene(identities,identityJson,identityError),"serialize identity reservations");
        se::World identityReload;Require(se::DeserializeScene(identityJson,identityReload,identityError)&&!identityReload.Create("reload reuse",deletedId),"retired identity survives scene reload");
        auto identityData=nlohmann::json::parse(identityJson);
        const auto rejectIdentity=[&](const nlohmann::json& data){
            Require(!se::DeserializeScene(data.dump(),identityReload,identityError),"invalid retired identities rejected");
            std::string after;Require(se::SerializeScene(identityReload,after,identityError)&&after==identityJson,"invalid identity load preserves all scene bytes");
        };
        auto badIdentity=identityData;badIdentity["retiredIds"]={true};rejectIdentity(badIdentity);
        badIdentity=identityData;badIdentity["retiredIds"]={"0"};rejectIdentity(badIdentity);
        badIdentity=identityData;badIdentity["retiredIds"]={std::to_string(deletedId),std::to_string(deletedId)};rejectIdentity(badIdentity);
        badIdentity=identityData;badIdentity["retiredIds"]={std::to_string(identities.Get(fresh)->stableId)};rejectIdentity(badIdentity);
        badIdentity=identityData;badIdentity["retiredIds"]=nlohmann::json::array();for(int index=0;index<100001;++index)badIdentity["retiredIds"].push_back(std::to_string(index+1));rejectIdentity(badIdentity);
        identityData.erase("retiredIds");se::World legacyIdentity;Require(se::DeserializeScene(identityData.dump(),legacyIdentity,identityError),"legacy v1 scene without retirement list accepted");
        Require(!identities.ReserveStableId(0),"zero reservation rejected");
        const auto maximum=identities.Create("maximum explicit ID",UINT64_MAX);Require(maximum&&identities.Destroy(maximum)&&identities.Create("automatic after maximum"),"explicit uint64 maximum does not poison automatic allocator");
        identities.ForEach([&](se::EntityHandle){Require(!identities.ReserveStableId(1234567),"reservation rejected during borrowed iteration");});
        Require(!world.SetTransform(replacement,{{},{1e30f,0,0,1},{1,1,1}}),"Overflowing quaternion norm rejected before normalization");
        const auto child=world.Create("child");
        Require(world.SetTransform(replacement,{{2,0,0},{},{2,2,2}})
            && world.SetTransform(child,{{1,0,0},{},{1,1,1}})
            && world.SetParent(child,replacement),"Build hierarchy");
        Require(Near(se::TransformPoint(world.GetWorldMatrix(child),{}),{4,0,0}),"Parent world matrix");
        Require(!world.SetParent(replacement,child) && world.LastError()=="World.ParentCycle","Reject parent cycle");
        Require(world.SetCamera(child,{se::Pi/3,0.1f,300,true}),"Camera component");
        Require(world.SetMesh(child,{"mesh-001",{"material-001"}}),"Mesh component");
        Require(world.SetLight(replacement,{{1,0.8f,0.6f},3}),"Light component");
        Require(!world.Create("duplicate",world.Get(child)->stableId),"Duplicate stable ID rejected");

        se::World copied=world;
        const auto copiedChild=copied.FindByStableId(world.Get(child)->stableId);
        Require(copiedChild && copiedChild!=child && !copied.IsAlive(child),"World copy owns different handles");
        Require(Near(se::TransformPoint(copied.GetWorldMatrix(copiedChild),{}),{4,0,0}),"World copy restores parent links");
        Require(copied.Destroy(copiedChild) && world.IsAlive(child),"Runtime copy is independent");

        se::World deferred;
        const auto first=deferred.Create("first");const auto second=deferred.Create("second");
        se::EntityHandle created;int visited=0;
        deferred.ForEach([&](se::EntityHandle entity)
        {
            ++visited;
            Require(deferred.Destroy(entity),"Queue destruction");
            Require(!deferred.IsAlive(entity)&&!deferred.Get(entity)&&!deferred.GetTransform(entity),"Destroy immediately invalidates the iteration target before physical release");
            if(entity==first)
            {
                created=deferred.Create("queued");
                Require(created && !deferred.IsAlive(created),"New entity reserved until boundary");
                Require(deferred.SetTransform(created,{{7,8,9},{},{1,1,1}}),"Queue transform after creation");
            }
            Require(!deferred.FlushCommands(),"Cannot flush during iteration");
        });
        Require(visited==2 && !deferred.IsAlive(first) && !deferred.IsAlive(second),"Stable iteration snapshot contains retired identities, never live targets");
        Require(deferred.FlushCommands() && deferred.Size()==1 && deferred.IsAlive(created)
            && Near(deferred.GetTransform(created)->position,{7,8,9}),"Deferred command order");
        se::EntityHandle afterClear;
        deferred.ForEach([&](se::EntityHandle)
        {
            deferred.Create("before clear");deferred.Clear();afterClear=deferred.Create("after clear");
            deferred.SetTransform(afterClear,{{1,2,3},{},{1,1,1}});
        });
        Require(deferred.FlushCommands()&&deferred.Size()==1&&deferred.IsAlive(afterClear)
            &&Near(deferred.GetTransform(afterClear)->position,{1,2,3}),"Deferred clear preserves later creates and edits");

        std::string text,error;
        Require(se::SerializeScene(world,text,error),"Serialize scene");
        se::World loaded;
        Require(se::DeserializeScene(text,loaded,error),"Load scene");
        const auto loadedChild=loaded.FindByStableId(world.Get(child)->stableId);
        Require(loadedChild && loaded.Get(loadedChild)->name=="child"
            && loaded.GetCamera(loadedChild)->primary && loaded.GetMesh(loadedChild)->meshAsset=="mesh-001"
            && Near(se::TransformPoint(loaded.GetWorldMatrix(loadedChild),{}),{4,0,0}),"Round trip components and references");
        std::string original;Require(se::SerializeScene(loaded,original,error),"Capture original scene");
        auto ExpectRejected=[&](const std::string& invalid)
        {
            Require(!se::DeserializeScene(invalid,loaded,error),"Invalid scene rejected");
            std::string after;Require(se::SerializeScene(loaded,after,error)&&after==original,"Failed load preserves world");
        };
        using Json=nlohmann::json;
        auto invalid=Json::parse(text);invalid["version"]=999;ExpectRejected(invalid.dump());
        invalid=Json::parse(text);invalid["entities"][1]["parent"]="999999999";ExpectRejected(invalid.dump());
        invalid=Json::parse(text);invalid["entities"][0]["parent"]=invalid["entities"][1]["id"];ExpectRejected(invalid.dump());
        invalid=Json::parse(text);invalid["entities"][1]["id"]=invalid["entities"][0]["id"];ExpectRejected(invalid.dump());
        invalid=Json::parse(text);invalid["entities"][0]["components"][0]["properties"]["rotation"]={0,0,0,0};ExpectRejected(invalid.dump());
        ExpectRejected("{broken");
        ExpectRejected("{\"format\":\"SelfEngine.Scene\",\"version\":1,\"version\":2,\"entities\":[]}");
        // 子实体在前也能通过第二阶段恢复父引用。
        auto reversed=Json::parse(text);std::swap(reversed["entities"][0],reversed["entities"][1]);
        Require(se::DeserializeScene(reversed.dump(),loaded,error),"Parent order independent");

        const auto path=output/"runtime.scene.json";
        Require(se::SaveScene(path,world,error),"Atomic initial save");
        const auto oldFile=Read(path);
        world.ForEach([&](se::EntityHandle){world.Create("pending");});
        Require(!se::SaveScene(path,world,error) && Read(path)==oldFile,"Failed save preserves prior file");
        Require(world.FlushCommands(),"Flush pending scene edits");
        Require(se::SaveScene(path,world,error) && se::LoadScene(path,loaded,error)
            && loaded.Size()==world.Size(),"Atomic replace and read");
        Require(!se::SaveScene(output/"missing-parent"/"scene.json",world,error),"Missing directory write rejected");
        const auto invalidFile=output/"broken.scene.json";{std::ofstream file(invalidFile);file<<"[broken";}
        const auto sizeBefore=loaded.Size();Require(!se::LoadScene(invalidFile,loaded,error)&&loaded.Size()==sizeBefore,"Bad file preserves current world");
        Require(world.Destroy(replacement) && !world.IsAlive(child),"Destroy parent removes subtree");
        std::cout<<"[PASS] Math, generation handles, hierarchy, deferred mutations, world copy and atomic scene IO\n";
        return 0;
    }
    catch(const std::exception& error)
    {std::cerr<<"[FAIL] "<<error.what()<<'\n';return 1;}
}
