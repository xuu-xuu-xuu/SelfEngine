#include "ToolLayer/Editor/AssetImportService.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,const std::string& text)
{
    fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(text.data(),static_cast<std::streamsize>(text.size()));Check(static_cast<bool>(output),"write fixture");
}
std::string Read(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
void Copy(const fs::path& source,const fs::path& target)
{fs::create_directories(target.parent_path());fs::copy_file(source,target,fs::copy_options::overwrite_existing);}
fs::path Metadata(const fs::path& source){auto result=source;result+=".asset.json";return result;}
const fs::path Samples=fs::path("Resources")/"Samples"/"Models";
void Image(const fs::path& target){Copy(Samples/"checker.png",target);}
void Replace(std::string& value,const std::string& from,const std::string& to)
{
    const auto at=value.find(from);Check(at!=std::string::npos,"fixture URI exists");value.replace(at,from.size(),to);
}
fs::path Model(const fs::path& directory,const std::string& name="Model.gltf")
{
    auto text=Read(Samples/"Hierarchy.gltf");
    Replace(text,"Hierarchy.bin","buffers/mesh.bin");Replace(text,"checker.png","textures/checker.png");
    const auto source=directory/name;Put(source,text);
    Copy(Samples/"Hierarchy.bin",directory/"buffers"/"mesh.bin");Image(directory/"textures"/"checker.png");
    Put(directory/"unused"/"never-import.txt","unreferenced sibling");Image(directory/"unused.png");
    return source;
}
struct Scenario
{
    fs::path content,incoming,cache;
    Scenario(const fs::path& output,const char* name)
        :content(output/name/"Content"),incoming(output/name/"incoming"),cache(output/name/"Cache")
    {fs::create_directories(content);fs::create_directories(incoming);}
    AssetImportRequest Request(std::vector<fs::path> sources,const fs::path& target={}) const
    {return {std::move(sources),content,target};}
};
AssetId FindId(const AssetStore& assets,const fs::path& source)
{
    for(const auto& id:assets.ListIds())if(fs::equivalent(assets.Resolve(id),source))return id;
    return {};
}
std::size_t FileCount(const fs::path& root)
{
    std::size_t result=0;for(const auto& item:fs::recursive_directory_iterator(root))if(item.is_regular_file())++result;
    return result;
}
void ClosureAndRegistration(const fs::path& output)
{
    Scenario test(output,"closure");const auto source=Model(test.incoming/"bundle");
    auto plan=AssetImportService::Plan(test.Request({source},"Models"));
    Check(plan.valid&&plan.files.size()==3&&plan.diagnostics.empty(),"model plan contains only three referenced files");
    Check(std::count_if(plan.files.begin(),plan.files.end(),[](const auto& file){return file.primary;})==1,"one primary model");
    Check(std::count_if(plan.files.begin(),plan.files.end(),[](const auto& file){return file.registerAsset;})==2,"model and PNG register, BIN does not");
    Check(std::all_of(plan.files.begin(),plan.files.end(),[](const auto& file){return file.copyRequired&&file.sourceDigest.size()==64;}),"frozen source hashes");
    const auto target=test.content/"Models"/"Model.gltf",buffer=test.content/"Models"/"buffers"/"mesh.bin",texture=test.content/"Models"/"textures"/"checker.png";
    AssetStore assets(test.content,test.cache);std::vector<AssetDiagnostic> diagnostics;
    Check(assets.Scan(diagnostics),"empty content scan");
    std::vector<AssetId> imported;std::string key,detail;
    Check(AssetImportService::ExecuteRegistered(plan,assets,imported,key,detail),"copy and register dependency closure");
    Check(key.empty()&&imported.size()==2&&assets.ListIds().size()==2,"registered IDs returned together");
    Check(Read(target)==Read(source)&&Read(buffer)==Read(source.parent_path()/"buffers"/"mesh.bin")
        &&Read(texture)==Read(source.parent_path()/"textures"/"checker.png"),"relative directories preserve exact source bytes");
    Check(FileCount(test.content)==5&&!fs::exists(test.content/"Models"/"unused")&&!fs::exists(test.content/"Models"/"unused.png"),"unreferenced siblings are not copied");
    Check(fs::exists(Metadata(target))&&fs::exists(Metadata(texture))&&!fs::exists(Metadata(buffer)),"only registrable files receive metadata");
    Check(!fs::exists(Metadata(source))&&!fs::exists(Metadata(source.parent_path()/"textures"/"checker.png")),"external source metadata untouched");
    const auto modelId=FindId(assets,target),imageId=FindId(assets,texture);
    Check(!modelId.empty()&&!imageId.empty(),"store resolves copied targets");
    Check(static_cast<bool>(assets.LoadModel(modelId,diagnostics)),"copied model really imports with dependencies");
    Check(static_cast<bool>(assets.LoadTextureImage(imageId,diagnostics)),"copied image really decodes");
    const auto repeated=AssetImportService::Plan(test.Request({source},"Models"));
    Check(!repeated.valid&&repeated.files.empty()&&repeated.conflicts.size()==1,"existing primary rejects whole repeated group");

    // 既有 sidecar 的用户字段、缩放选项、格式与 ID 均不能在原地重登记时被重写。
    auto metadata=nlohmann::json::parse(Read(Metadata(target)));
    metadata["options"]["scale"]=2.5f;metadata["userMarker"]="preserve exactly";
    Put(Metadata(target),metadata.dump(4)+"\n");
    const auto previousMetadata=Read(Metadata(target)),previousImageMetadata=Read(Metadata(texture));
    Check(assets.Scan(diagnostics),"rescan changed project metadata");
    const auto local=AssetImportService::Plan(test.Request({target},"IgnoredForExistingSources"));
    Check(local.valid&&local.files.size()==3&&std::none_of(local.files.begin(),local.files.end(),[](const auto& file){return file.copyRequired;}),"project source reimport stays in place");
    Check(AssetImportService::ExecuteRegistered(local,assets,imported,key,detail),"project source registration succeeds");
    Check(FindId(assets,target)==modelId&&FindId(assets,texture)==imageId&&assets.ListIds().size()==2,"project source IDs are stable");
    Check(Read(Metadata(target))==previousMetadata&&Read(Metadata(texture))==previousImageMetadata,"existing sidecars byte-preserved");
    AssetSourceSnapshot snapshot;
    Check(assets.DescribeSource(modelId,snapshot,diagnostics)&&snapshot.options.scale==2.5f,"existing import settings preserved");
    Check(!fs::exists(test.content/"IgnoredForExistingSources"),"project reimport creates no target folder");
}
void CopyOnlyAndSharedSources(const fs::path& output)
{
    Scenario copy(output,"copy-only");const auto source=Model(copy.incoming);
    const auto plan=AssetImportService::Plan(copy.Request({source},"Models"));
    std::string key,detail;
    Check(AssetImportService::Execute(plan,key,detail),"legacy copy-only entry executes closure");
    Check(FileCount(copy.content)==3&&!fs::exists(Metadata(copy.content/"Models"/"Model.gltf")),"copy-only entry does not register");

    Scenario shared(output,"shared");const auto first=Model(shared.incoming,"A.gltf"),second=Model(shared.incoming,"B.gltf");
    const auto combined=AssetImportService::Plan(shared.Request({first,second,first}));
    Check(combined.valid&&combined.files.size()==4&&combined.conflicts.empty(),"same-source dependencies and repeated inputs deduplicate");
    Check(std::count_if(combined.files.begin(),combined.files.end(),[](const auto& file){return file.primary;})==2,"shared closure keeps both primary models");
    AssetStore assets(shared.content,shared.cache);std::vector<AssetId> imported;
    Check(AssetImportService::ExecuteRegistered(combined,assets,imported,key,detail)&&imported.size()==3,"shared image registers once");
    Check(FileCount(shared.content)==7,"two models, two dependencies, three metadata files");
}
void ConflictingGroups(const fs::path& output)
{
    Scenario test(output,"conflicting-groups");
    const auto first=Model(test.incoming/"first","A.gltf"),second=Model(test.incoming/"second","B.gltf");
    Image(test.incoming/"independent.png");
    const auto plan=AssetImportService::Plan(test.Request({first,second,test.incoming/"independent.png"}));
    Check(plan.valid&&plan.files.size()==1&&plan.conflicts.size()==2&&plan.diagnostics.size()==2,"different sources claiming a dependency reject both groups");
    Check(std::all_of(plan.diagnostics.begin(),plan.diagnostics.end(),[](const auto& diagnostic){return diagnostic.key=="Assets.ImportConflict";}),"per-group conflict keys");
    AssetStore assets(test.content,test.cache);std::vector<AssetId> imported;std::string key,detail;
    Check(AssetImportService::ExecuteRegistered(plan,assets,imported,key,detail)&&imported.size()==1,"independent group still imports");
    Check(FileCount(test.content)==2&&!fs::exists(test.content/"A.gltf")&&!fs::exists(test.content/"B.gltf")&&!fs::exists(test.content/"buffers"),"rejected groups leave no partial copies");

    Scenario names(output,"case-collision");Image(names.incoming/"one"/"Asset.png");Image(names.incoming/"two"/"asset.png");
    const auto colliding=AssetImportService::Plan(names.Request({names.incoming/"one"/"Asset.png",names.incoming/"two"/"asset.png"}));
    Check(!colliding.valid&&colliding.conflicts.size()==2&&colliding.files.empty(),"Windows case-insensitive target collision");
}
void InvalidDependenciesAndTargets(const fs::path& output)
{
    Scenario missing(output,"missing-texture");const auto source=Model(missing.incoming/"model");
    fs::remove(source.parent_path()/"textures"/"checker.png");Image(missing.incoming/"good.png");
    const auto partial=AssetImportService::Plan(missing.Request({source,missing.incoming/"good.png"}));
    Check(partial.valid&&partial.files.size()==1&&partial.unsupported.size()==1,"missing dependency rejects entire group but keeps valid input");
    Check(partial.diagnostics.size()==1&&partial.diagnostics[0].key=="Assets.ImportDependencyMissing","missing optional importer texture becomes import-plan failure");
    std::string key,detail;
    Check(AssetImportService::Execute(partial,key,detail)&&FileCount(missing.content)==1,"no main or buffer from rejected missing-texture group");

    Scenario buffer(output,"missing-buffer");const auto broken=Model(buffer.incoming);fs::remove(buffer.incoming/"buffers"/"mesh.bin");
    const auto invalid=AssetImportService::Plan(buffer.Request({broken}));
    Check(!invalid.valid&&invalid.files.empty()&&invalid.unsupported.size()==1,"missing binary buffer rejects primary and every dependency");
    Put(buffer.incoming/"bad.gltf","{not json}");Put(buffer.incoming/"bad.png","not an image");Put(buffer.incoming/"note.txt","text");
    const auto malformed=AssetImportService::Plan(buffer.Request({buffer.incoming/"bad.gltf",buffer.incoming/"bad.png",buffer.incoming/"note.txt",buffer.incoming/"missing.fbx"}));
    Check(!malformed.valid&&malformed.unsupported.size()==4,"actual parsers reject malformed models and images");

    Scenario target(output,"dependency-target-exists");const auto model=Model(target.incoming);
    Put(target.content/"Models"/"buffers"/"mesh.bin","existing user buffer");
    const auto conflict=AssetImportService::Plan(target.Request({model},"Models"));
    Check(!conflict.valid&&conflict.files.empty()&&conflict.conflicts.size()==1,"existing dependency target rejects whole group");
    Check(Read(target.content/"Models"/"buffers"/"mesh.bin")=="existing user buffer","target content preserved during planning");

    Scenario sidecar(output,"sidecar-target-exists");const auto sidecarModel=Model(sidecar.incoming);
    const auto existing=Metadata(sidecar.content/"Models"/"textures"/"checker.png");Put(existing,"existing user sidecar");
    const auto metadataConflict=AssetImportService::Plan(sidecar.Request({sidecarModel},"Models"));
    Check(!metadataConflict.valid&&metadataConflict.files.empty()&&metadataConflict.conflicts.size()==1,"orphan dependency sidecar rejects whole group");
    Check(Read(existing)=="existing user sidecar"&&!fs::exists(sidecar.content/"Models"/"Model.gltf"),"existing sidecar never overwritten");
}
void FrozenSourcesAndRaces(const fs::path& output)
{
    Scenario changed(output,"changed-source");Image(changed.incoming/"image.png");
    const auto plan=AssetImportService::Plan(changed.Request({changed.incoming/"image.png"}));Check(plan.valid,"source-change plan valid");
    Put(changed.incoming/"image.png",Read(changed.incoming/"image.png")+"changed after confirmation");
    std::string key,detail;
    Check(!AssetImportService::Execute(plan,key,detail)&&key=="Assets.ImportSourceChanged"&&FileCount(changed.content)==0,"source digest change refuses before writing");

    Scenario dependency(output,"changed-dependency");const auto source=Model(dependency.incoming);
    const auto closure=AssetImportService::Plan(dependency.Request({source}));Check(closure.valid,"dependency-change plan valid");
    Put(dependency.incoming/"buffers"/"mesh.bin",Read(dependency.incoming/"buffers"/"mesh.bin")+"changed");
    AssetStore dependencyStore(dependency.content,dependency.cache);std::vector<AssetId> imported;
    Check(!AssetImportService::ExecuteRegistered(closure,dependencyStore,imported,key,detail)&&key=="Assets.ImportSourceChanged"
        &&FileCount(dependency.content)==0&&dependencyStore.ListIds().empty(),"dependency digest freezes with primary");

    Scenario occupied(output,"occupied-target");Image(occupied.incoming/"image.png");
    const auto race=AssetImportService::Plan(occupied.Request({occupied.incoming/"image.png"}));Check(race.valid,"target-race plan valid");
    Put(occupied.content/"image.png","created after confirmation");
    AssetStore occupiedStore(occupied.content,occupied.cache);
    Check(!AssetImportService::ExecuteRegistered(race,occupiedStore,imported,key,detail)&&key=="Assets.ImportConflict","occupied target refuses transaction");
    Check(Read(occupied.content/"image.png")=="created after confirmation"&&!fs::exists(Metadata(occupied.content/"image.png"))&&occupiedStore.ListIds().empty(),"race winner preserved without sidecar or index residue");

    Scenario metadata(output,"occupied-sidecar");Image(metadata.incoming/"image.png");
    const auto sidecarRace=AssetImportService::Plan(metadata.Request({metadata.incoming/"image.png"}));Check(sidecarRace.valid,"sidecar-race plan valid");
    Put(Metadata(metadata.content/"image.png"),"created sidecar after confirmation");
    Check(!AssetImportService::Execute(sidecarRace,key,detail)&&key=="Assets.ImportConflict","occupied sidecar refuses copy-only transaction too");
    Check(!fs::exists(metadata.content/"image.png")&&Read(Metadata(metadata.content/"image.png"))=="created sidecar after confirmation","sidecar race winner preserved");

    Scenario existing(output,"changed-project-sidecar");Image(existing.content/"image.png");
    AssetStore existingStore(existing.content,existing.cache);AssetId existingId;std::vector<AssetDiagnostic> diagnostics;
    Check(existingStore.RegisterSource(existing.content/"image.png",{},existingId,diagnostics),"register existing source");
    const auto local=AssetImportService::Plan(existing.Request({existing.content/"image.png"}));Check(local.valid,"existing project source plan valid");
    const auto revised=Read(Metadata(existing.content/"image.png"))+"\n";Put(Metadata(existing.content/"image.png"),revised);
    Check(!AssetImportService::ExecuteRegistered(local,existingStore,imported,key,detail)&&key=="Assets.ImportSourceChanged","changed project sidecar invalidates confirmation");
    Check(Read(Metadata(existing.content/"image.png"))==revised&&existingStore.ListIds()==std::vector<AssetId>{existingId},"changed project metadata and index remain intact");
}
void RegistrationRollback(const fs::path& output)
{
    Scenario test(output,"registration-rollback");
    Image(test.content/"keep.png");Image(test.content/"unregistered.png");Image(test.content/"bad.png");Image(test.incoming/"fresh.png");
    AssetStore assets(test.content,test.cache);AssetId keepId;std::vector<AssetDiagnostic> diagnostics;
    Check(assets.RegisterSource(test.content/"keep.png",{},keepId,diagnostics),"register original retained asset");
    const auto keepBytes=Read(test.content/"keep.png"),keepMetadata=Read(Metadata(test.content/"keep.png"));
    const auto localBytes=Read(test.content/"unregistered.png"),badBytes=Read(test.content/"bad.png");
    const std::string invalidMetadata="{\"version\":999,\"userData\":\"must survive\"}";Put(Metadata(test.content/"bad.png"),invalidMetadata);
    const auto plan=AssetImportService::Plan(test.Request({test.incoming/"fresh.png",test.content/"unregistered.png",test.content/"bad.png"},"Imported"));
    Check(plan.valid&&plan.files.size()==3,"existing bad metadata is retained for registration validation");
    std::vector<AssetId> imported={"previous caller value"};std::string key,detail;
    Check(!AssetImportService::ExecuteRegistered(plan,assets,imported,key,detail)&&key=="Assets.ImportRegistrationFailed","registration scan failure rolls back copied and newly registered files");
    Check(imported.empty()&&assets.ListIds()==std::vector<AssetId>{keepId}&&assets.Resolve(keepId)==fs::weakly_canonical(test.content/"keep.png"),"rollback restores original index even when pre-existing invalid metadata prevents Scan");
    Check(!fs::exists(test.content/"Imported")&&!fs::exists(Metadata(test.content/"unregistered.png")),"rollback removes only new copied file, new sidecars and empty created directories");
    Check(Read(test.content/"keep.png")==keepBytes&&Read(Metadata(test.content/"keep.png"))==keepMetadata
        &&Read(test.content/"unregistered.png")==localBytes&&Read(test.content/"bad.png")==badBytes
        &&Read(Metadata(test.content/"bad.png"))==invalidMetadata,"rollback preserves all pre-existing bytes");

    Scenario local(output,"local-new-registration");Image(local.content/"image.png");const auto bytes=Read(local.content/"image.png");
    const auto localPlan=AssetImportService::Plan(local.Request({local.content/"image.png"},"Ignored"));
    Check(localPlan.valid&&localPlan.files.size()==1&&!localPlan.files[0].copyRequired,"unregistered project image is an in-place registration");
    AssetStore localStore(local.content,local.cache);
    Check(AssetImportService::ExecuteRegistered(localPlan,localStore,imported,key,detail)&&imported.size()==1,"in-place registration adds only owned metadata");
    Check(Read(local.content/"image.png")==bytes&&fs::exists(Metadata(local.content/"image.png"))&&!fs::exists(local.content/"Ignored"),"in-place registration leaves source in place");
}
void Boundaries(const fs::path& output)
{
    Scenario test(output,"boundaries");Image(test.incoming/"image.png");
    const auto source=test.incoming/"image.png";
    Check(AssetImportService::SupportedExtension("model.glb")&&AssetImportService::SupportedExtension("x.material.json")
        &&AssetImportService::SupportedExtension("shader.sebin")&&!AssetImportService::SupportedExtension("note.txt"),"extension support policy");
    Check(AssetImportService::Plan(test.Request({})).errorKey=="Assets.ImportNoFiles","empty request rejected");
    auto invalidRoot=test.Request({source});invalidRoot.contentRoot=test.content/"absent";
    Check(AssetImportService::Plan(invalidRoot).errorKey=="Assets.ImportContentRoot","nonexistent content root rejected");
    Check(AssetImportService::Plan(test.Request({source},"..")).errorKey=="Assets.ReferenceOutsideRoot","escaping target directory rejected");
    Check(AssetImportService::Plan(test.Request({source},test.content/"Absolute")).errorKey=="Assets.ReferenceOutsideRoot","absolute targetDirectory violates relative contract");
    auto tampered=AssetImportService::Plan(test.Request({source}));Check(tampered.valid,"boundary test plan valid");
    tampered.files[0].target=output/"outside.png";std::string key,detail;
    Check(!AssetImportService::Execute(tampered,key,detail)&&key=="Assets.ReferenceOutsideRoot"&&!fs::exists(output/"outside.png"),"execution independently validates target boundary");
    auto valid=AssetImportService::Plan(test.Request({source}));
    Scenario other(output,"other-store");AssetStore wrongStore(other.content,other.cache);std::vector<AssetId> imported;
    Check(!AssetImportService::ExecuteRegistered(valid,wrongStore,imported,key,detail)&&key=="Assets.ImportContentRoot"&&FileCount(test.content)==0,"registration store must own planned content root");
    AssetImportPlan empty;
    Check(!AssetImportService::Execute(empty,key,detail)&&key=="Assets.ImportNothingSupported","invalid plan execution rejected");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: AssetImportTests <output-dir>");
        const auto unique=asset_io::NewId();Check(!unique.empty(),"unique output identity");
        const auto output=fs::absolute(fs::path(argv[1]))/("asset-import-"+unique);
        fs::create_directories(output); // 不清空或删除调用者传入的目录。
        ClosureAndRegistration(output);CopyOnlyAndSharedSources(output);ConflictingGroups(output);
        InvalidDependenciesAndTargets(output);FrozenSourcesAndRaces(output);RegistrationRollback(output);Boundaries(output);
        for(const auto& item:fs::recursive_directory_iterator(output))
            Check(item.path().filename().wstring().find(L".tmp-")==std::wstring::npos,"no transaction temporary files left behind");
        std::cout<<"AssetImportTests passed (real model closure, image registration, group conflicts, frozen sources, boundaries and registration rollback).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"AssetImportTests failed: "<<failure.what()<<'\n';return 1;
    }
}
