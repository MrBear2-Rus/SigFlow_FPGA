#include "WaveformBackendTraceSource.h"

#include <utility>

namespace sigflow {
namespace trace {

WaveformBackendTraceSource::WaveformBackendTraceSource(
    std::shared_ptr<eda::IWaveformBackend> backend)
    : backend_(std::move(backend)) {}

bool WaveformBackendTraceSource::Open(const std::string& path, std::string& error) {
    if (!backend_) {
        error = "no waveform backend configured";
        return false;
    }
    if (!backend_->Open(path, error)) return false;

    path_ = path;
    timescale_ = backend_->Timescale();
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
