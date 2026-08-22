#pragma once

#include "../trace/TraceSource.h"
#include "WaveViewState.h"

#include <string>

namespace sigflow {
namespace wave {

// A-B 区间测量结果（W3-02 本地统计）。
struct WaveMeasurement {
    bool valid = false;
    std::uint64_t edgeCount = 0;      // A..B 内跳变次数
    std::uint64_t xzCount = 0;        // x/z 跳变次数
    std::uint64_t glitchCount = 0;    // 窄脉冲数（相邻跳变间隔 <= 阈值）
    double avgPeriod = 0.0;           // 上升沿平均周期
    double dutyCycle = 0.0;           // 高电平占比（0..1）
    std::string message;
};

// 对单个信号统计 [a, b] 区间：边沿数、x/z、毛刺、周期、占空比。
// glitchThreshold：两次跳变间隔小于等于该值视为窄脉冲。
bool MeasureSignal(sigflow::trace::TraceSource& source,
                   const sigflow::trace::SignalInfo& signal,
                   sigflow::trace::TimeValue a, sigflow::trace::TimeValue b,
                   sigflow::trace::TimeValue glitchThreshold,
                   WaveMeasurement& out);

} // namespace wave
} // namespace sigflow
