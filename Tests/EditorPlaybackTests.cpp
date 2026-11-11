#include "ToolLayer/Editor/EditorPlayback.h"
#include "ToolLayer/Editor/EditorSession.h"
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int wmain()
{
    try
    {
        EditorPlaybackController playback;std::string error;
        Check(playback.State()==EditorPlaybackState::Editing,"initial editing");
        Check(!playback.Pause(error)&&error=="Editor.PlaybackInvalidState","pause rejected in editing");
        Check(!playback.Stop(error)&&error=="Editor.PlaybackInvalidState","stop rejected in editing");
        Check(playback.Play(error)&&playback.State()==EditorPlaybackState::Preparing,"play enters preparing");
        Check(!playback.Play(error)&&error=="Editor.PlaybackBusy","second play rejected");
        Check(!playback.Step(error)&&error=="Editor.PlaybackInvalidState","step rejected before pausing");
        Check(playback.Failed(error)&&playback.State()==EditorPlaybackState::Editing,"failure returns to editing");
        Check(playback.Play(error)&&playback.Prepared(error)&&playback.State()==EditorPlaybackState::Playing,"prepared enters playing");
        Check(!playback.Step(error)&&error=="Editor.PlaybackInvalidState","step rejected while playing");
        Check(playback.Pause(error)&&playback.State()==EditorPlaybackState::Paused,"pause enters paused");
        Check(playback.Step(error)&&playback.Steps()==1&&playback.Step(error)&&playback.Steps()==2,"step advances while paused");
        Check(!playback.Play(error)&&error=="Editor.PlaybackBusy","play rejected while paused");
        Check(playback.Resume(error)&&playback.State()==EditorPlaybackState::Playing,"resume enters playing");
        Check(playback.Stop(error)&&playback.State()==EditorPlaybackState::Stopping,"stop enters stopping");
        Check(!playback.Stop(error)&&error=="Editor.PlaybackInvalidState","double stop rejected");
        Check(playback.Stopped(error)&&playback.State()==EditorPlaybackState::Editing,"stopped returns to editing");
        Check(playback.Steps()==2,"steps preserved until reset");
        playback.Reset();Check(playback.Steps()==0&&playback.State()==EditorPlaybackState::Editing,"reset clears steps");
        // 运行环境单步：暂停后恰好一个固定步，步后仍保持暂停。
        EditorSession session;
        Check(!session.SetPlayPaused(true)&&session.LastError()=="Editor.PlaybackInvalidState","pause requires a play world");
        std::string validation;
        Check(!session.ValidatePlay(validation)&&validation=="Editor.PlayNoCamera","empty edit world needs a camera");
        Check(session.Apply("Editor.PlayCamera",[](World& world){
            const auto camera=world.Create("Camera",201);
            return camera&&world.SetCamera(camera,{});
        }),"camera fixture");
        Check(session.ValidatePlay(validation),"camera accepted before play");
        Check(session.StartPlay(),"session starts play");
        const auto before=session.PlayEnvironment()->FixedStepIndex();
        Check(!session.StepPlayFrame()&&session.LastError()=="Editor.PlaybackInvalidState","step requires paused play");
        Check(session.SetPlayPaused(true)&&session.PlayPaused(),"play environment pauses");
        Check(session.StepPlayFrame(),"paused step executes");
        Check(session.PlayEnvironment()->FixedStepIndex()==before+1,"step advances exactly one fixed step");
        Check(session.PlayPaused(),"step keeps play paused");
        Check(session.SetPlayPaused(false)&&!session.PlayPaused(),"resume clears pause");
        session.StopPlay();Check(!session.IsPlaying(),"stop clears play world");
        Check(session.PlayFixedStep()>0,"fixed step duration remains available");
        std::cout<<"EditorPlaybackTests passed (play/pause/step/stop transitions, invalid requests and one fixed step).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"EditorPlaybackTests failed: "<<failure.what()<<'\n';return 1;
    }
}
