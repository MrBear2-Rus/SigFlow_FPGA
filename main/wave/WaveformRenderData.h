#pragma once

#include "../trace/TraceSource.h"
#include "WaveViewState.h"

#include <string>
#include <utility>
#include <vector>

namespace sigflow {
namespace wave {

struct WaveColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

const WaveColor& WavePalette(int index);

struct WaveformPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct WaveformSignalData {
    int id = -1;
    std::string name;
    bool vector = false;
    std::vector<WaveformPoint> polyline;
    std::vector<std::pair<sigflow::trace::TimeValue, std::string>> valueLabels;
};

struct WaveformTick {
    sigflow::trace::TimeValue time = 0;
    std::string label;
};

struct WaveformFrame {
    std::vector<WaveformSignalData> signals;
    std::vector<WaveformTick> ticks;
    std::string error;
};

// 构建一帧渲染数据（软件 / GL 后端共用）。
bool BuildWaveformFrame(sigflow::trace::TraceSource& source,
                        const WaveViewState& state,
                        int width, int height,
                        WaveformFrame& out);

// 时间 → 绘图区 X 坐标（网格线、文本层共用）。
float WaveformTimeToX(const WaveViewState& state, int width,
                      sigflow::trace::TimeValue t);

} // namespace wave
} // namespace sigflow
