#include "WaveformRenderData.h"

#include <cmath>

namespace sigflow {
namespace wave {

namespace {

float RowCenter(const WaveViewState& state, int row)
{
    return static_cast<float>(state.headerHeight + row * state.rowHeight +
                              state.rowHeight / 2);
}

float ScalarY(const WaveViewState& state, int row, const std::string& value)
{
    const float center = RowCenter(state, row);
    if (value == "1") return center - 6.0f;
    if (value == "0") return center + 6.0f;
    return center; // x / z / 未知
}

} // namespace

const WaveColor& WavePalette(int index)
{
    static const WaveColor palette[] = {
        { 59.0f / 255.0f, 130.0f / 255.0f, 246.0f / 255.0f },  // 蓝
        { 34.0f / 255.0f, 197.0f / 255.0f, 94.0f / 255.0f },   // 绿
        { 234.0f / 255.0f, 179.0f / 255.0f, 8.0f / 255.0f },   // 黄
        { 239.0f / 255.0f, 68.0f / 255.0f, 68.0f / 255.0f },   // 红
        { 168.0f / 255.0f, 85.0f / 255.0f, 247.0f / 255.0f },  // 紫
        { 251.0f / 255.0f, 146.0f / 255.0f, 60.0f / 255.0f },  // 橙
        { 45.0f / 255.0f, 212.0f / 255.0f, 191.0f / 255.0f },  // 青
        { 244.0f / 255.0f, 114.0f / 255.0f, 182.0f / 255.0f }, // 粉
    };
    return palette[index % (sizeof(palette) / sizeof(palette[0]))];
}

float WaveformTimeToX(const WaveViewState& state, int width,
                      sigflow::trace::TimeValue t)
{
    const double ratio = static_cast<double>(t - state.timeOffset) /
                         static_cast<double>(state.timeSpan);
    return static_cast<float>(state.leftMargin +
        ratio * (width - state.leftMargin - state.rightMargin));
}

bool BuildWaveformFrame(sigflow::trace::TraceSource& source,
                        const WaveViewState& state,
                        int width, int height,
                        WaveformFrame& out)
{
    out.signals.clear();
    out.ticks.clear();
    out.error.clear();
    if (!state.valid || width <= state.leftMargin + state.rightMargin) {
        out.error = "invalid view state or window too small";
        return false;
    }

    const sigflow::trace::TimeValue t0 = state.timeOffset;
    const sigflow::trace::TimeValue t1 = state.EndTime();

    // 时间轴刻度：{1,2,5}×10^k，目标像素间距 >= 90
    {
        const int plotWidth = width - state.leftMargin - state.rightMargin;
        const double pixelsPerTime = static_cast<double>(plotWidth) /
                                     static_cast<double>(state.timeSpan);
        const double target = 90.0 / pixelsPerTime;
        double step = 1.0;
        while (step < target) step *= 10.0;
        for (double candidate : { step / 2.0, step / 5.0, step }) {
            if (candidate >= target) { step = candidate; break; }
        }
        const sigflow::trace::TimeValue stepT = static_cast<sigflow::trace::TimeValue>(step);
        sigflow::trace::TimeValue tick =
            (t0 / stepT) * stepT + ((t0 % stepT == 0) ? 0 : stepT);
        for (; tick <= t1; tick += stepT) {
            WaveformTick entry;
            entry.time = tick;
            entry.label = std::to_string(tick);
            out.ticks.push_back(std::move(entry));
        }
    }

    out.signals.reserve(state.visibleSignalIds.size());
    int row = 0;
    for (int signalId : state.visibleSignalIds) {
        const sigflow::trace::SignalInfo* info = source.SignalById(signalId);
        if (!info) continue;

        WaveformSignalData data;
        data.id = info->id;
        data.name = info->name;
        data.vector = (info->kind == sigflow::trace::SignalKind::Vector ||
                       info->kind == sigflow::trace::SignalKind::Real ||
                       info->kind == sigflow::trace::SignalKind::String);

        std::vector<sigflow::trace::Transition> transitions;
        std::string error;
        if (!source.Query(*info, t0, t1, transitions, error)) {
            out.error = error;
            return false;
        }

        std::string initial;
        if (!source.ValueAt(*info, t0, initial, error)) {
            out.error = error;
            return false;
        }
        if (initial.empty()) initial = "x";

        if (!data.vector) {
            const float startX = WaveformTimeToX(state, width, t0);
            const float endX = WaveformTimeToX(state, width, t1);
            data.polyline.push_back({ startX, ScalarY(state, row, initial) });
            std::string last = initial;
            float lastX = startX;
            for (const sigflow::trace::Transition& tr : transitions) {
                const float x = WaveformTimeToX(state, width, tr.time);
                if (tr.value == last) {
                    if (x > lastX + 0.5f) {
                        data.polyline.push_back({ x, ScalarY(state, row, last) });
                        lastX = x;
                    }
                    continue;
                }
                data.polyline.push_back({ x, ScalarY(state, row, last) });
                data.polyline.push_back({ x, ScalarY(state, row, tr.value) });
                data.valueLabels.push_back({ tr.time, tr.value });
                last = tr.value;
                lastX = x;
            }
            data.polyline.push_back({ endX, ScalarY(state, row, last) });
        } else {
            const float center = RowCenter(state, row);
            data.polyline.push_back({ WaveformTimeToX(state, width, t0), center });
            data.polyline.push_back({ WaveformTimeToX(state, width, t1), center });
            for (const sigflow::trace::Transition& tr : transitions) {
                data.valueLabels.push_back({ tr.time, tr.value });
            }
            if (data.valueLabels.empty()) {
                data.valueLabels.push_back({ t0, initial });
            }
        }
        out.signals.push_back(std::move(data));
        ++row;
    }
    return true;
}

} // namespace wave
} // namespace sigflow
