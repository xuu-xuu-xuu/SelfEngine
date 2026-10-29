#include "FrameTimer.h"

bool InitializeFrameTimer(FrameTimer& timer)
{
    timer = FrameTimer{};

    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (!QueryPerformanceFrequency(&frequency)
        || frequency.QuadPart <= 0
        || !QueryPerformanceCounter(&counter))
    {
        return false;
    }

    timer.frequency = frequency;
    timer.previousCounter = counter;
    return true;
}

bool UpdateFrameTimer(FrameTimer& timer)
{
    if (timer.frequency.QuadPart <= 0)
    {
        return false;
    }

    LARGE_INTEGER currentCounter{};
    if (!QueryPerformanceCounter(&currentCounter))
    {
        return false;
    }

    // 先求计数差，再转换为浮点数做除法，结果单位为秒。
    timer.deltaSeconds =
        static_cast<double>(currentCounter.QuadPart - timer.previousCounter.QuadPart)
        / static_cast<double>(timer.frequency.QuadPart);
    timer.previousCounter = currentCounter;
    timer.totalSeconds += timer.deltaSeconds;
    ++timer.frameCount;
    return true;
}
