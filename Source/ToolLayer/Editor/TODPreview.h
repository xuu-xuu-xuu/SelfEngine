#pragma once
#include "FunctionLayer/Runtime/Environment.h"
#include "ResourceLayer/Assets/AssetStore.h"

namespace se
{
// 预览独占时钟、消息与运行状态，资源发布只保存配置，不接触任何 Play 上下文。
class TODPreview
{
public:
    bool Open(AssetStore& assets,const AssetId& id);
    bool Apply(const TODProfile& candidate);
    bool Save();
    bool SetTime(double time){return environment_.TimeOfDay().SetTime(0,time);}
    bool Advance(double seconds){return environment_.RunFrame(seconds);}
    void SetPaused(bool paused){environment_.SetPaused(paused);}
    bool Paused()const{return environment_.Paused();}
    const TODProfile& Profile()const{return profile_;}
    const AssetId& Id()const{return id_;}
    bool Ready()const{return !id_.empty();}
    bool Dirty()const;
    EnvironmentSnapshot Snapshot()const{return environment_.Snapshot();}
    const std::string& LastError()const{return error_;}
private:
    Environment environment_;
    TODProfile profile_;
    AssetId id_;std::filesystem::path path_;
    std::string saved_,error_;
};
}
