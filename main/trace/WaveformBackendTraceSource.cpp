#include "WaveformBackendTraceSource.h"

#include <utility>
#include <fstream>
#include <algorithm>

namespace sigflow {
namespace trace {

WaveformBackendTraceSource::WaveformBackendTraceSource(
    std::shared_ptr<eda::IWaveformBackend> backend)
    : backend_(std::move(backend)) {}

bool WaveformBackendTraceSource::Open(const std::string& path, std::string& error) {
    timeDomain_ = TraceTimeDomain::Unknown;
    timescale_.clear();
    if (!backend_) {
        error = "no waveform backend configured";
        return false;
    }
    if (!backend_->Open(path, error)) return false;

    path_ = path;
    timescale_ = backend_->Timescale();
    std::ifstream timingInput(path);
    std::string timingHeader, timingLine;
    while (timingHeader.size() < 8192 && std::getline(timingInput, timingLine)) {
        timingHeader += timingLine + "\n";
        if (timingLine.find("$enddefinitions") != std::string::npos) break;
    }
    // A backend may supply a display default for a missing VCD timescale.
    // Only the file declaration can establish a physical unit for this adapter.
    const auto declaration = timingHeader.find("$timescale");
    const auto declarationEnd = declaration == std::string::npos ? std::string::npos
        : timingHeader.find("$end", declaration);
    if (declarationEnd == std::string::npos) {
        timescale_.clear();
    } else {
        timescale_ = timingHeader.substr(declaration + 10, declarationEnd - declaration - 10);
        timescale_.erase(std::remove_if(timescale_.begin(), timescale_.end(),
            [](unsigned char c) { return std::isspace(c); }), timescale_.end());
        std::uint64_t tick = 0;
        if (!VcdTickFemtoseconds(timescale_, tick)) timescale_.clear();
    }
    timeDomain_ = ClassifyVcdTime(timingHeader, timescale_);
    signals_.clear();
    for (const auto& signal : backend_->Signals()) {
        SignalInfo info;
        info.id = signal.id;
        info.name = signal.name;
        info.scope = signal.scope;
        info.fullName = signal.fullName;
        info.idCode = signal.idCode;
        info.width = signal.width;
        info.kind = signal.width > 1 ? SignalKind::Vector : SignalKind::Scalar;
        signals_.push_back(std::move(info));
    }

    const eda::WaveTimeRange range = backend_->TimeRange();
    timeRange_.begin = range.begin;
    timeRange_.end = range.end;
    timeRange_.valid = range.valid;
    return true;
}

const SignalInfo* WaveformBackendTraceSource::SignalById(int id) const {
    for (const auto& signal : signals_) {
        if (signal.id == id) return &signal;
    }
    return nullptr;
}

TraceTimeRange WaveformBackendTraceSource::TimeRange() const { return timeRange_; }

bool WaveformBackendTraceSource::Query(const SignalInfo& signal, TimeValue t0, TimeValue t1,
                                       std::vector<Transition>& out, std::string& error) {
    out.clear();
    if (!backend_) {
        error = "no waveform backend configured";
        return false;
    }
    std::vector<eda::WaveTransition> transitions;
    if (!backend_->Query(signal.id, t0, t1, transitions, error)) return false;
    out.reserve(transitions.size());
    for (const auto& transition : transitions) {
        out.push_back({transition.time, transition.value});
    }
    return true;
}

bool WaveformBackendTraceSource::ValueAt(const SignalInfo& signal, TimeValue t,
                                         std::string& value, std::string& error) {
    if (!backend_) {
        error = "no waveform backend configured";
        return false;
    }
    return backend_->ValueAt(signal.id, t, value, error);
}

} // namespace trace
} // namespace sigflow
