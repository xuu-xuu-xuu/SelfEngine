#pragma once

struct InputState;
struct FrameTimer;

// 验证程序的只读日志观察器；不修改输入、计时或窗口生命周期。
class SandboxDiagnostics
{
public:
    explicit SandboxDiagnostics(const InputState& initialInput);

    void ObserveInput(const InputState& input);
    void ObserveTiming(const FrameTimer& timer, double updateDeltaSeconds);

private:
    bool previousWDown_ = false;
    double lastTimingLogSeconds_ = 0.0;
};
