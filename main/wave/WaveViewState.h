#pragma once

#include "../trace/TraceTypes.h"
#include "WaveTheme.h"

#include <limits>
#include <vector>

namespace sigflow {
namespace wave {

// 命名 Marker（W3-02）。
struct WaveMarker {
    sigflow::trace::TimeValue time = 0;
    std::string label;
    std::uint32_t color = 0xFF3B82F6; // ARGB
};

// 事件时间线条目（W3-03，来源：TraceBridge compare.json 等）。
struct WaveEvent {
    sigflow::trace::TimeValue time = 0;
    std::string label;
    std::uint32_t color = 0xFFF59E0B;
    std::string signalName;
    std::string sourcePath;
    int sourceLine = 0;
};

// 波形视图状态（软件/GL 后端共享）。
struct WaveViewState {
    std::vector<int> visibleSignalIds;
    sigflow::trace::TimeValue timeOffset = 0;
    sigflow::trace::TimeValue timeSpan = 1000;
    sigflow::trace::TimeValue maxTime = 0;
    int leftMargin = 180;
    int rightMargin = 40;
    int headerHeight = 36;
    int rowHeight = 30;
    bool valid = false;

    // W3：播放头 / Marker / A-B 测量 / 事件
    sigflow::trace::TimeValue playhead = 0;
    bool hasPlayhead = false;
    std::vector<WaveMarker> markers;
    std::vector<WaveEvent> events;
    sigflow::trace::TimeValue abA = 0;
    sigflow::trace::TimeValue abB = 0;
    bool hasAB = false;
    WaveTheme theme = WaveTheme::Dark;

    sigflow::trace::TimeValue EndTime() const { return timeOffset + timeSpan; }

    void Clamp()
    {
        if (timeSpan < 1) timeSpan = 1;
        const sigflow::trace::TimeValue maxValue =
            std::numeric_limits<sigflow::trace::TimeValue>::max();
        const sigflow::trace::TimeValue fullSpan =
            (maxTime == maxValue) ? maxValue : maxTime + 1;
        if (timeSpan > fullSpan) timeSpan = fullSpan;
        if (timeOffset > maxTime) timeOffset = maxTime;
        const sigflow::trace::TimeValue maxOffset =
            (maxTime > timeSpan) ? (maxTime - timeSpan) : 0;
        if (timeOffset > maxOffset) timeOffset = maxOffset;
        valid = maxTime > 0 || !visibleSignalIds.empty();
    }

    void SetRange(sigflow::trace::TimeValue a, sigflow::trace::TimeValue b)
    {
        if (a > b) std::swap(a, b);
        abA = a;
        abB = b;
        hasAB = true;
    }
};

namespace WaveViewInteraction {

inline void Clamp(WaveViewState& state) { state.Clamp(); }

inline void ZoomAt(WaveViewState& state, double mouseXFrac, double factor)
{
    if (factor <= 0.0) return;
    if (mouseXFrac < 0.0) mouseXFrac = 0.0;
    if (mouseXFrac > 1.0) mouseXFrac = 1.0;
    const sigflow::trace::TimeValue anchor =
        state.timeOffset + static_cast<sigflow::trace::TimeValue>(
            static_cast<double>(state.timeSpan) * mouseXFrac);
    sigflow::trace::TimeValue newSpan =
        static_cast<sigflow::trace::TimeValue>(
            static_cast<double>(state.timeSpan) * factor);
    if (newSpan < 1) newSpan = 1;
    if (factor > 1.0 && newSpan <= state.timeSpan &&
        state.timeSpan < std::numeric_limits<sigflow::trace::TimeValue>::max()) {
        newSpan = state.timeSpan + 1;
    }
    const sigflow::trace::TimeValue newOffset =
        (anchor > newSpan) ? (anchor - newSpan) : 0;
    state.timeSpan = newSpan;
    state.timeOffset = newOffset;
    state.Clamp();
}

inline void ZoomStep(WaveViewState& state, bool zoomIn, double centerFrac = 0.5)
{
    ZoomAt(state, centerFrac, zoomIn ? 0.7 : 1.45);
}

inline void PanBy(WaveViewState& state, double viewportFrac)
{
    const sigflow::trace::TimeValue delta = static_cast<sigflow::trace::TimeValue>(
        static_cast<double>(state.timeSpan) * viewportFrac);
    if (delta > state.timeOffset) {
        state.timeOffset = 0;
    } else {
        state.timeOffset -= delta;
    }
    state.Clamp();
}

inline void Reset(WaveViewState& state)
{
    state.timeOffset = 0;
    state.timeSpan = (state.maxTime + 1 > 1000) ? state.maxTime + 1 : 1000;
    state.Clamp();
}

inline void JumpToTime(WaveViewState& state, sigflow::trace::TimeValue t)
{
    if (t > state.timeOffset && t < state.EndTime()) return; // 已在视口内
    const sigflow::trace::TimeValue half = state.timeSpan / 2;
    state.timeOffset = (t > half) ? (t - half) : 0;
    state.Clamp();
}

inline sigflow::trace::TimeValue TimeAtX(const WaveViewState& state, int width, int x)
{
    const int plotWidth = width - state.leftMargin - state.rightMargin;
    if (plotWidth <= 0) return state.timeOffset;
    const double ratio = static_cast<double>(x - state.leftMargin) /
                         static_cast<double>(plotWidth);
    if (ratio < 0.0) return state.timeOffset;
    if (ratio > 1.0) return state.EndTime();
    return state.timeOffset +
        static_cast<sigflow::trace::TimeValue>(ratio * static_cast<double>(state.timeSpan));
}

} // namespace WaveViewInteraction

} // namespace wave
} // namespace sigflow
