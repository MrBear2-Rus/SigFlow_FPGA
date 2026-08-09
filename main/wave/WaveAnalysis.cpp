#include "WaveAnalysis.h"

#include <vector>

namespace sigflow {
namespace wave {

bool MeasureSignal(sigflow::trace::TraceSource& source,
                   const sigflow::trace::SignalInfo& signal,
                   sigflow::trace::TimeValue a, sigflow::trace::TimeValue b,
                   sigflow::trace::TimeValue glitchThreshold,
                   WaveMeasurement& out)
{
    out = WaveMeasurement();
    if (b < a) {
        out.message = "invalid range";
        return false;
    }

    std::vector<sigflow::trace::Transition> transitions;
    std::string error;
    if (!source.Query(signal, a, b, transitions, error)) {
        out.message = error;
        return false;
    }

    std::string initial;
    if (!source.ValueAt(signal, a, initial, error)) {
        out.message = error;
        return false;
    }
    if (initial.empty()) initial = "x";

    out.edgeCount = transitions.size();
    out.xzCount = 0;
    out.glitchCount = 0;

    sigflow::trace::TimeValue previousTime = a;
    std::string previousValue = initial;
    std::vector<sigflow::trace::TimeValue> risingEdges;
    std::uint64_t highTime = 0;
    const sigflow::trace::TimeValue range = b - a;

    for (const sigflow::trace::Transition& tr : transitions) {
        if (tr.value == "x" || tr.value == "z" || tr.value == "X" || tr.value == "Z") {
            ++out.xzCount;
        }
        const sigflow::trace::TimeValue delta = tr.time - previousTime;
        if (delta <= glitchThreshold && delta > 0) {
            ++out.glitchCount;
        }
        if (previousValue == "1") {
            highTime += delta;
        }
        if (previousValue != "1" && tr.value == "1") {
            risingEdges.push_back(tr.time);
        }
        previousTime = tr.time;
        previousValue = tr.value;
    }
    if (previousValue == "1") {
        highTime += (b - previousTime);
    }

    if (out.edgeCount > 0) {
        out.avgPeriod = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 1; i < risingEdges.size(); ++i) {
            out.avgPeriod += static_cast<double>(risingEdges[i] - risingEdges[i - 1]);
            ++count;
        }
        if (count > 0) out.avgPeriod /= static_cast<double>(count);
    }
    if (range > 0) {
        out.dutyCycle = static_cast<double>(highTime) / static_cast<double>(range);
    }
    out.valid = true;
    return true;
}

} // namespace wave
} // namespace sigflow
