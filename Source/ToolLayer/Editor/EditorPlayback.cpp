#include "EditorPlayback.h"

namespace se
{
bool EditorPlaybackController::Fail(std::string& errorKey,const char* key)
{
    errorKey=key;return false;
}
bool EditorPlaybackController::Play(std::string& errorKey)
{
    errorKey.clear();
    if(state_!=EditorPlaybackState::Editing)return Fail(errorKey,"Editor.PlaybackBusy");
    state_=EditorPlaybackState::Preparing;return true;
}
bool EditorPlaybackController::Prepared(std::string& errorKey)
{
    errorKey.clear();
    if(state_!=EditorPlaybackState::Preparing)return Fail(errorKey,"Editor.PlaybackInvalidState");
    state_=EditorPlaybackState::Playing;return true;
}
bool EditorPlaybackController::Failed(std::string& errorKey)
{
    errorKey.clear();
    if(state_!=EditorPlaybackState::Preparing)return Fail(errorKey,"Editor.PlaybackInvalidState");
    state_=EditorPlaybackState::Editing;return true;
}
bool EditorPlaybackController::Pause(std::string& errorKey)
{
    errorKey.clear();
    if(state_!=EditorPlaybackState::Playing)return Fail(errorKey,"Editor.PlaybackInvalidState");
    state_=EditorPlaybackState::Paused;return true;
}
bool EditorPlaybackController::Resume(std::string& errorKey)
{
    errorKey.clear();
    if(state_!=EditorPlaybackState::Paused)return Fail(errorKey,"Editor.PlaybackInvalidState");
    state_=EditorPlaybackState::Playing;return true;
}
bool EditorPlaybackController::Step(std::string& errorKey)
{
    errorKey.clear();
    if(state_!=EditorPlaybackState::Paused)return Fail(errorKey,"Editor.PlaybackInvalidState");
    ++steps_;return true;
}
bool EditorPlaybackController::Stop(std::string& errorKey)
{
    errorKey.clear();
    if(state_==EditorPlaybackState::Editing||state_==EditorPlaybackState::Stopping)return Fail(errorKey,"Editor.PlaybackInvalidState");
    state_=EditorPlaybackState::Stopping;return true;
}
bool EditorPlaybackController::Stopped(std::string& errorKey)
{
    errorKey.clear();
    if(state_!=EditorPlaybackState::Stopping)return Fail(errorKey,"Editor.PlaybackInvalidState");
    state_=EditorPlaybackState::Editing;return true;
}
void EditorPlaybackController::Reset() noexcept
{
    state_=EditorPlaybackState::Editing;steps_=0;
}
}
