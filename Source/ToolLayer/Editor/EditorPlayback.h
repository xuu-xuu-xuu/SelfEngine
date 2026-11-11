#pragma once
#include <cstdint>
#include <string>

namespace se
{
enum class EditorPlaybackState { Editing, Preparing, Playing, Paused, Stopping };
// 编辑/运行状态机：Editing -> Preparing -> Playing <-> Paused -> Stopping -> Editing。
// 运行世界的创建与清理仍由 EditorSession/EditorApplication 负责，本类只约束状态与重复请求。
class EditorPlaybackController
{
public:
    EditorPlaybackState State() const noexcept{return state_;}
    bool Play(std::string& errorKey);
    bool Prepared(std::string& errorKey);
    bool Failed(std::string& errorKey);
    bool Pause(std::string& errorKey);
    bool Resume(std::string& errorKey);
    bool Step(std::string& errorKey);
    bool Stop(std::string& errorKey);
    bool Stopped(std::string& errorKey);
    std::uint64_t Steps() const noexcept{return steps_;}
    void Reset() noexcept;
private:
    bool Fail(std::string& errorKey,const char* key);
    EditorPlaybackState state_=EditorPlaybackState::Editing;
    std::uint64_t steps_=0;
};
}
