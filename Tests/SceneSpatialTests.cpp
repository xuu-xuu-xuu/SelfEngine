#include "FunctionLayer/Runtime/Environment.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using Timer=std::chrono::steady_clock;
template<class F> double Measure(F&& operation){const auto start=Timer::now();operation();return std::chrono::duration<double,std::milli>(Timer::now()-start).count();}
// 独立段-AABB SAT 参考，不调用树的射线 slab 函数。
bool SegmentOracle(const Aabb& box,const spatial::Ray& ray)
{
    const double length=std::sqrt(double(ray.direction.x)*ray.direction.x+double(ray.direction.y)*ray.direction.y+double(ray.direction.z)*ray.direction.z);
    const double direction[]{ray.direction.x,ray.direction.y,ray.direction.z},origin[]{ray.origin.x,ray.origin.y,ray.origin.z};
    const double low[]{box.minimum.x,box.minimum.y,box.minimum.z},high[]{box.maximum.x,box.maximum.y,box.maximum.z};
    double d[3]{},m[3]{},e[3]{},ad[3]{};
    for(int axis=0;axis<3;++axis){d[axis]=direction[axis]/length*ray.maximumDistance*.5;m[axis]=origin[axis]+d[axis]-(low[axis]+high[axis])*.5;e[axis]=(high[axis]-low[axis])*.5;ad[axis]=std::abs(d[axis]);if(std::abs(m[axis])>e[axis]+ad[axis])return false;}
    for(int axis=0;axis<3;++axis){const int b=(axis+1)%3,c=(axis+2)%3;if(std::abs(m[b]*d[c]-m[c]*d[b])>e[b]*ad[c]+e[c]*ad[b])return false;}
    return true;
}
bool BoxOracle(const Aabb& a,const Aabb& b)
{return !(a.maximum.x<b.minimum.x||a.minimum.x>b.maximum.x||a.maximum.y<b.minimum.y||a.minimum.y>b.maximum.y||a.maximum.z<b.minimum.z||a.minimum.z>b.maximum.z);}
SceneDocument Document(std::uint64_t id)
{World world{World::StagingTag{}};world.Create("new scene",id);std::string text,error;SceneDocument result;Check(SerializeScene(world,text,error)&&ParseSceneDocument(text,result,error),"scene fixture");return result;}
}
int main(int argc,char** argv)
{
    try
    {
        const Aabb unit{{-.5f,-.5f,-.5f},{.5f,.5f,.5f}},all{{-10000,-10000,-10000},{10000,10000,10000}};
        spatial::BoundsIndex fixed,dynamic;Check(fixed.Query(all).empty()&&dynamic.Validate(),"empty trees");
        Check(dynamic.Upsert({1,unit})&&dynamic.Query(Aabb{{.5f,.5f,.5f},{.5f,.5f,.5f}})==std::vector<std::uint64_t>{1},"closed boundary contact");
        const auto point=spatial::PrepareRay({{0,0,0},{1,0,0},0});Check(point&&dynamic.Query(*point)==std::vector<std::uint64_t>{1},"zero-length ray inside box");
        Check(!spatial::PrepareRay({{},{},10})&&!spatial::PrepareRay({{},{1,0,0},-1}),"invalid rays rejected");
        const auto nan=std::numeric_limits<float>::quiet_NaN();
        Check(!dynamic.Upsert({2,{{nan,0,0},{1,1,1}}})&&!spatial::PrepareRay({{nan,0,0},{1,0,0},1})&&dynamic.Statistics().leaves==1,"non-finite input cannot enter index");
        std::mt19937 random(6062026);std::uniform_real_distribution<float> position(-100,100),extent(.05f,3);
        std::vector<spatial::Leaf> leaves;
        for(std::uint64_t i=1;i<=2048;++i){const Vec3 center{position(random),position(random),position(random)},size{extent(random),extent(random),extent(random)};leaves.push_back({i,{center-size,center+size}});}
        const auto staticBuild=Measure([&]{Check(fixed.Build(leaves),"static median build");});dynamic.Clear();
        const auto dynamicBuild=Measure([&]{for(const auto& leaf:leaves)Check(dynamic.Upsert(leaf),"dynamic insert");});
        Check(fixed.Validate()&&dynamic.Validate(),"initial structure invariants");
        auto invalid=leaves;invalid.back().key=invalid.front().key;const auto before=fixed.Query(all);Check(!fixed.Build(invalid)&&fixed.Query(all)==before,"invalid rebuild preserves previous tree");
        std::vector<Aabb> queries;std::vector<spatial::Ray> rays;
        for(int query=0;query<512;++query){Vec3 center{position(random),position(random),position(random)};queries.push_back({center-Vec3{5,5,5},center+Vec3{5,5,5}});rays.push_back({center,{position(random),position(random),position(random)},250});}
        std::uint64_t nodeTests=0,candidates=0;
        for(std::size_t query=0;query<queries.size();++query)
        {
            std::vector<std::uint64_t> boxes,rayHits;
            for(const auto& leaf:leaves){if(BoxOracle(leaf.bounds,queries[query]))boxes.push_back(leaf.key);if(SegmentOracle(leaf.bounds,rays[query]))rayHits.push_back(leaf.key);}
            const auto prepared=spatial::PrepareRay(rays[query]);std::size_t visited=0;
            Check(fixed.Query(queries[query],&visited)==boxes&&dynamic.Query(queries[query])==boxes,"range query matches independent linear oracle");nodeTests+=visited;candidates+=boxes.size();
            Check(prepared&&fixed.Query(*prepared)==rayHits&&dynamic.Query(*prepared)==rayHits,"ray query matches segment SAT oracle");
        }
        double coreQuery=Measure([&]{for(const auto& query:queries)candidates+=fixed.Query(query).size();});
        double linearQuery=Measure([&]{for(const auto& query:queries)for(const auto& leaf:leaves)if(BoxOracle(leaf.bounds,query))++candidates;});
        double dynamicUpdate=Measure([&]{for(std::size_t i=0;i<leaves.size();i+=3){leaves[i].bounds.minimum.x+=15;leaves[i].bounds.maximum.x+=15;Check(dynamic.Upsert(leaves[i]),"moving dynamic leaf");}});
        for(std::size_t i=0;i<leaves.size();i+=5)Check(dynamic.Remove(leaves[i].key),"dynamic removal");
        std::erase_if(leaves,[](const auto& leaf){return (leaf.key-1)%5==0;});Check(dynamic.Validate(),"refit and removal invariants");
        for(const auto& query:queries){std::vector<std::uint64_t> expected;for(const auto& leaf:leaves)if(BoxOracle(leaf.bounds,query))expected.push_back(leaf.key);Check(dynamic.Query(query)==expected,"updated tree oracle");}
        spatial::BoundsIndex overlap,ordered;for(std::uint64_t i=1;i<=256;++i){Check(overlap.Upsert({i,unit}),"overlapping leaves");Check(ordered.Upsert({i,{{float(i),0,0},{float(i)+1,1,1}}}),"ordered leaves");}
        Check(overlap.Query(unit).size()==256&&overlap.Validate()&&ordered.Validate()&&ordered.Statistics().height<=20,"dense overlap and bounded degenerate tree height");

        Environment environment;auto& world=environment.GetWorld();auto& index=environment.Spatial();auto& scenes=environment.Scenes();
        const auto parent=world.Create("parent",100),child=world.Create("child",101);Check(world.SetParent(child,parent)&&world.SetTransform(child,{{3,0,0},{},{1,1,1}}),"hierarchy fixture");
        Check(index.Insert(parent,unit,SpatialClass::Static)&&index.Insert(child,unit),"runtime static and dynamic entries");
        auto agree=[&]{for(const auto& box:queries)Check(index.QueryAabb(box)==index.QueryAabb(box,{},true),"runtime fresh range oracle");for(const auto& ray:rays)Check(index.RaycastCandidates(ray)==index.RaycastCandidates(ray,{},true),"runtime fresh ray oracle");};agree();
        Check(world.SetTransform(parent,{{10,0,0},{},{1,1,1}}),"moving static ancestor");
        const Aabb aroundChild{{12.4f,-1,-1},{13.6f,1,1}};Check(index.QueryAabb(aroundChild)==std::vector<EntityHandle>{child},"parent change invalidates child world bounds");
        Check(index.UpdateBounds(child,{{-2,-2,-2},{2,2,2}})&&index.SetClassification(child,SpatialClass::Static),"shape and classification changes");agree();
        Check(world.SetEnabled(parent,false)&&index.QueryAabb(all).empty(),"disabled ancestor removes leaves before next frame");Check(world.SetEnabled(parent,true)&&index.QueryAabb(all).size()==2,"enabled descendants return");
        const auto previous=index.WorldBounds(child);Check(!index.UpdateBounds(child,{{1,0,0},{-1,0,0}})&&index.LastError()=="Spatial.InvalidBounds"&&index.WorldBounds(child)->minimum.x==previous->minimum.x,"invalid bounds preserve valid state");
        const auto oldChild=child;Check(world.Destroy(child)&&index.QueryAabb(all).size()==1,"destroy removes leaf");const auto reused=world.Create("reused slot",102);
        Check(reused.index==oldChild.index&&reused.generation!=oldChild.generation&&index.Insert(reused,unit)&&index.QueryAabb(all).size()==2,"slot reuse cannot resolve stale leaf");
        const auto scene=scenes.ActiveScene();Check(scenes.Unload(scene)&&index.QueryAabb(all).empty()&&environment.RunFrame(0)&&index.Statistics().tracked==0,"scene unload removes members and index");
        const auto a=scenes.Create("A"),b=scenes.Create("B");Check(scenes.RequestDocument(a,Document(1000))&&environment.RunFrame(0),"new scene activation");const auto aEntity=world.FindByStableId(1000);Check(index.Insert(aEntity,unit),"active scene leaf");
        Check(scenes.RequestDocument(b,Document(2000),SceneLoadMode::Additive)&&environment.RunFrame(0)&&index.Insert(world.FindByStableId(2000),unit),"additive leaf");
        Check(index.QueryAabb(all,a)==std::vector<EntityHandle>{aEntity}&&index.QueryAabb(all,b).size()==1,"scene query filters membership");
        Check(scenes.RequestDocument(a,Document(1000))&&environment.RunFrame(0)&&index.QueryAabb(all,a).empty()&&!world.IsAlive(aEntity),"reload never transfers old leaf to new generation");
        Check(index.Insert(world.FindByStableId(1000),unit)&&index.QueryAabb(all).size()==2,"new generation explicitly registers bounds");
        const auto diagnostics=environment.Spatial().Statistics();environment.Clear();Check(index.Statistics().tracked==0,"clear removes spatial state");

        Environment physical;auto& pw=physical.GetWorld();const auto actor=pw.Create("box");Check(physical.Spatial().Insert(actor,unit),"physical candidate");
        PhysicsWorld physics;BodyDesc description;description.mass=0;description.entityStableId=pw.Get(actor)->stableId;const auto body=physics.CreateBody(description);Check(bool(body),"actual box");
        const auto precise=physics.Raycast({0,0,3},{0,0,-1},10);Check(precise&&std::abs(precise->distance-2.5f)<1e-5f&&physical.Spatial().RaycastCandidates({{0,0,3},{0,0,-1},10})==std::vector<EntityHandle>{actor},"broadphase candidate followed by actual shape raycast");
        EnvironmentHooks hooks;hooks.physics=[&](double){return pw.SetTransform(actor,{{4,0,0},{},{1,1,1}});};
        bool afterPhysics=false;hooks.phase=[&](EnvironmentPhase phase,const TickContext&){if(phase==EnvironmentPhase::AfterPhysics)afterPhysics=physical.Spatial().QueryAabb({{3,-1,-1},{5,1,1}})==std::vector<EntityHandle>{actor};if(phase==EnvironmentPhase::PostTick)return pw.SetTransform(actor,{{8,0,0},{},{1,1,1}});return true;};
        Check(physical.RunFrame(1.0/60,hooks)&&afterPhysics&&physical.Spatial().WorldBounds(actor)->minimum.x>7,"physics boundary and PostTick freshness");

        Environment overflow;auto& ow=overflow.GetWorld();const auto hugeParent=ow.Create("huge parent"),hugeChild=ow.Create("huge child");
        Check(ow.SetParent(hugeChild,hugeParent)&&ow.SetTransform(hugeParent,{{},{},{1e20f,1e20f,1e20f}})&&ow.SetTransform(hugeChild,{{},{},{1e20f,1e20f,1e20f}})&&overflow.Spatial().Insert(hugeChild,unit),"finite local transforms with overflowing world product");
        Check(!overflow.Spatial().WorldBounds(hugeChild)&&overflow.Spatial().QueryAabb({{999,999,999},{1000,1000,1000}})==std::vector<EntityHandle>{hugeChild},"invalid world bounds stay conservative candidates");
        Environment benchmark;auto& bw=benchmark.GetWorld();std::vector<EntityHandle> objects;
        for(std::size_t i=0;i<2048;++i){auto entity=bw.Create("indexed",10000+i);const auto x=float(i%32)*4,z=float(i/32)*4;bw.SetTransform(entity,{{x,0,z},{},{1,1,1}});Check(benchmark.Spatial().Insert(entity,unit,i%2?SpatialClass::Dynamic:SpatialClass::Static),"benchmark leaf");objects.push_back(entity);}
        const auto runtimeBuild=Measure([&]{Check(benchmark.Spatial().Synchronize(),"initial spatial sync");});
        std::vector<Aabb> runtimeQueries;for(int i=0;i<512;++i){const float x=float(i%32)*4,z=float((i*13)%64)*4;runtimeQueries.push_back({{x-2,-1,z-2},{x+2,1,z+2}});}
        std::vector<std::vector<EntityHandle>> treeResults;const auto runtimeTree=Measure([&]{for(const auto& query:runtimeQueries)treeResults.push_back(benchmark.Spatial().QueryAabb(query));});
        const auto runtimeLinear=Measure([&]{for(std::size_t i=0;i<runtimeQueries.size();++i)Check(benchmark.Spatial().QueryAabb(runtimeQueries[i],{},true)==treeResults[i],"benchmark identical candidates");});
        const auto runtimeUpdate=Measure([&]{for(std::size_t i=0;i<objects.size();i+=7){auto transform=*bw.GetTransform(objects[i]);transform.position.x+=2;bw.SetTransform(objects[i],transform);}Check(benchmark.Spatial().Synchronize(),"mixed static/dynamic update");});
        for(const auto& query:runtimeQueries)Check(benchmark.Spatial().QueryAabb(query)==benchmark.Spatial().QueryAabb(query,{},true),"moved benchmark oracle");
        const auto stats=benchmark.Spatial().Statistics();Check(stats.synchronizations==2&&stats.scanned==4096&&stats.tracked==2048,"unchanged queries avoid dirty scans");
        const std::filesystem::path proof=argc>1?argv[1]:"out/spatial/verification.json";std::filesystem::create_directories(proof.parent_path());
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"seed",6062026},{"leaves",2048},{"queryCases",512},{"independentSegmentOracle",true},{"boundary",true},{"invalidPreserves",true},{"hierarchyFresh",true},{"classification",true},{"slotReuse",true},{"sceneUnload",true},{"reloadGeneration",true},{"exactBoxRay",true},{"fixedAndPostTickFresh",true},{"nodeTests",nodeTests},{"candidatesChecksum",candidates},{"staticBuildMs",staticBuild},{"dynamicBuildMs",dynamicBuild},{"dynamicUpdateMs",dynamicUpdate},{"coreTreeQueryMs",coreQuery},{"coreLinearQueryMs",linearQuery},{"runtimeBuildMs",runtimeBuild},{"runtimeTreeQueryMs",runtimeTree},{"runtimeLinearQueryMs",runtimeLinear},{"runtimeUpdateMs",runtimeUpdate},{"conservativeOverflow",true},{"runtimeStaticRebuildMs",stats.rebuildMilliseconds},{"runtimeDynamicRebuildMs",stats.dynamicRebuildMilliseconds},{"coreDynamicRebuildMs",dynamic.Statistics().rebuildMilliseconds},{"runtimeSyncMs",stats.synchronizeMilliseconds},{"staticHeight",stats.staticHeight},{"dynamicHeight",stats.dynamicHeight},{"dynamicRebuilds",stats.dynamicRebuilds},{"runtimeRemoved",diagnostics.removed}}.dump(2);
        std::cout<<"SceneSpatial passed (independent range/ray oracle, hierarchy, lifetime, fresh boundaries and measured costs)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
