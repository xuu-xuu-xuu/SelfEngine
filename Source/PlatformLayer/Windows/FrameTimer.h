#pragma once

#include <cstdint>
#include <Windows.h>

struct FrameTimer
{
    LARGE_INTEGER frequency{};       // 计数器每秒增加多少次计数。
    LARGE_INTEGER previousCounter{};
    double deltaSeconds = 0.0;       // 相邻两次更新时间之间的秒数。
    double totalSeconds = 0.0;
    std::uint64_t frameCount = 0;     // 成功更新时间的次数，不是显示帧数。
};

// 清零统计并取得计数器频率和起点；成功返回 true。
bool InitializeFrameTimer(FrameTimer& timer);

// 初始化成功后，每轮更新调用一次；读取失败时返回 false。
bool UpdateFrameTimer(FrameTimer& timer);
