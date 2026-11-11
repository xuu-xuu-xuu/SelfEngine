#pragma once
#include "LoadingWorkload.h"
#include "FunctionLayer/Rendering/AssetUploadScheduler.h"
class Localization;
namespace se
{
struct LoadingExperimentConfig
{
    std::filesystem::path workload,output,capture;
    std::string sourceIdentity;
    bool asynchronous=true,show=false;
    std::uint32_t frames=480,triggerFrame=30,frameMilliseconds=16,cancelFrame=0,restartFrame=0;
    AssetUploadBudget budget{256*1024,4};
};
int RunLoadingExperiment(const LoadingExperimentConfig& config,const Localization& text);
}
