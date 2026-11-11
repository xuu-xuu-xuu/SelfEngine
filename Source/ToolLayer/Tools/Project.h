#pragma once
#include "CoreLayer/Math/Math.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
struct ProjectDiagnostic {std::string key,detail;};
struct ProjectAsset {std::string id,type;};
struct ProjectBody {std::uint64_t entity=0;Vec3 halfExtents{0.5f,0.5f,0.5f};float mass=1,friction=0.6f,restitution=0;};
struct ProjectImpulse {std::uint32_t step=0;std::uint64_t entity=0;Vec3 impulse{},offset{};};
struct ProjectDescription
{
    std::filesystem::path root,manifest,contentRoot,entryScene;
    std::string name;
    std::vector<std::filesystem::path> scenes,uiDocuments;
    std::vector<ProjectAsset> assets;
    std::uint32_t replaySteps=600;
    std::vector<ProjectBody> bodies;
    std::vector<ProjectImpulse> impulses;
};
struct ProjectValidation
{
    ProjectDescription project;
    std::vector<std::filesystem::path> dependencies; // 规范化后项目根内的绝对文件路径，已排序去重。
    std::size_t entities=0,uiNodes=0;
};
struct ReplayReport {std::uint32_t steps=0;std::string stateHash;std::size_t entities=0;};
bool ReadProject(const std::filesystem::path& manifest,ProjectDescription& output,std::vector<ProjectDiagnostic>& diagnostics);
bool ValidateProject(const std::filesystem::path& manifest,ProjectValidation& output,std::vector<ProjectDiagnostic>& diagnostics);
bool CreateProject(const std::filesystem::path& templateDirectory,const std::filesystem::path& destination,std::vector<ProjectDiagnostic>& diagnostics);
bool ExportProjectDependencies(const std::filesystem::path& manifest,const std::filesystem::path& output,std::vector<ProjectDiagnostic>& diagnostics);
bool ReplayProject(const std::filesystem::path& manifest,const std::filesystem::path& outputDirectory,ReplayReport& report,
    std::vector<ProjectDiagnostic>& diagnostics,std::uint32_t stepsOverride=0,bool profile=true);
}
