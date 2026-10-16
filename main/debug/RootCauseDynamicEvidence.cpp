#include "RootCauseDynamicEvidence.h"

#include "../trace/TraceTimebase.h"

#include <algorithm>
#include <limits>
#include <map>
#include <sstream>

namespace sigflow::debug {
namespace {

using sigflow::trace::SignalInfo;
using sigflow::trace::TimeValue;
using sigflow::trace::TraceSource;
using sigflow::trace::Transition;

const SignalInfo* ResolveSignal(TraceSource& source, const std::string& key,
                                std::string& error)
{
    const SignalInfo* found = nullptr;
    for (const auto& signal : source.Signals()) {
        if (signal.fullName != key) continue;
        if (found != nullptr) {
            error = "dynamic evidence signal path is ambiguous: " + key;
            return nullptr;
        }
        found = &signal;
    }
    if (!found) error = "dynamic evidence signal path is missing: " + key;
    return found;
}

const SignalInfo* ResolveOptionalSignal(TraceSource& source, int id,
                                        const char* role, std::string& error)
{
    if (id < 0) return nullptr;
    const SignalInfo* signal = source.SignalById(id);
    if (!signal) error = std::string("dynamic evidence missing ") + role + " signal";
    return signal;
}

bool ReadWindow(TraceSource& source, const SignalInfo& signal,
                TimeValue start, TimeValue end,
                std::vector<Transition>& transitions,
                std::string& initial, std::string& error)
{
    if (!source.ValueAt(signal, start, initial, error)) return false;
    if (!source.Query(signal, start, end, transitions, error)) return false;
    std::sort(transitions.begin(), transitions.end(),
              [](const Transition& left, const Transition& right) {
                  return left.time < right.time;
              });
    return true;
}

bool IsBit(const std::string& value, bool& bit)
{
    if (value == "0") { bit = false; return true; }
    if (value == "1") { bit = true; return true; }
    return false;
}

bool FindResetRelease(TraceSource& source, const SignalInfo* reset,
                      TimeValue start, TimeValue end, bool activeHigh,
                      TimeValue& releaseTime, std::string& error)
{
    if (!reset) return false;
    std::string initial;
    std::vector<Transition> transitions;
    if (!ReadWindow(source, *reset, start, end, transitions, initial, error)) return false;
    bool previous = false;
    if (!IsBit(initial, previous)) return false;
    const bool active = activeHigh;
    for (const auto& transition : transitions) {
        bool current = false;
        if (!IsBit(transition.value, current)) continue;
        if (previous == active && current != active) {
            releaseTime = transition.time;
            return true;
        }
        previous = current;
    }
    return false;
}

bool FindInterfaceAnomaly(TraceSource& source, const SignalInfo* valid,
                          const SignalInfo* ready, TimeValue time,
                          bool& anomaly, std::string& error)
{
    anomaly = false;
    if (!valid || !ready) return true;
    bool validBit = false, readyBit = false;
    std::string validValue, readyValue;
    if (!source.ValueAt(*valid, time, validValue, error) ||
        !source.ValueAt(*ready, time, readyValue, error)) return false;
    if (IsBit(validValue, validBit) && IsBit(readyValue, readyBit)) {
        anomaly = validBit && !readyBit;
    }
    return true;
}

} // namespace

bool ExtractRootCauseDynamicEvidence(
    sigflow::trace::TraceSource& source,
    const std::vector<std::string>& candidateSignals,
    const RootCauseDynamicEvidenceOptions& options,
    std::vector<RootCauseEvidenceObservation>& out,
    std::string& error)
{
    error.clear();
    out.clear();
    if (options.captureId.empty() || options.analysisId.empty()) {
        error = "dynamic evidence requires capture_id and analysis_id";
        return false;
    }
    if (candidateSignals.empty() || candidateSignals.size() > 1024) {
        error = "dynamic evidence requires 1..1024 candidate signal paths";
        return false;
    }
    if (options.preWindow == 0 && options.postWindow == 0) {
        error = "dynamic evidence window must be non-zero";
        return false;
    }
    if (options.requirePhysicalTime &&
        source.TimeDomain() != sigflow::trace::TraceTimeDomain::Physical) {
        error = "TIME_DOMAIN_UNKNOWN: dynamic evidence requires physical VCD time";
        return false;
    }
    std::uint64_t tickFs = 0;
    if (!sigflow::trace::VcdTickFemtoseconds(source.Timescale(), tickFs)) {
        error = "TIME_DOMAIN_UNKNOWN: invalid or missing VCD timescale";
        return false;
    }
    const auto range = source.TimeRange();
    if (!range.valid || options.symptomTime < range.begin || options.symptomTime > range.end) {
        error = "dynamic evidence symptom time is outside the trace";
        return false;
    }
    const TimeValue start = options.symptomTime > options.preWindow
        ? options.symptomTime - options.preWindow : range.begin;
    const TimeValue remaining = range.end - options.symptomTime;
    const TimeValue post = std::min(options.postWindow, remaining);
    const TimeValue end = options.symptomTime + post;
    const SignalInfo* reset = ResolveOptionalSignal(source, options.resetSignalId,
                                                    "reset", error);
    const SignalInfo* valid = ResolveOptionalSignal(source, options.validSignalId,
                                                    "valid", error);
    const SignalInfo* ready = ResolveOptionalSignal(source, options.readySignalId,
                                                    "ready", error);
    if (!error.empty()) return false;

    TimeValue resetRelease = 0;
    const bool resetWasReleased = FindResetRelease(source, reset, start, end,
                                                    options.resetActiveHigh,
                                                    resetRelease, error);
    if (!error.empty()) return false;
    bool interfaceAnomaly = false;
    if (!FindInterfaceAnomaly(source, valid, ready, options.symptomTime,
                              interfaceAnomaly, error)) return false;

    TimeValue alignedSymptom = options.symptomTime;
    if (options.latencyKnown && options.pipelineLatency <= alignedSymptom) {
        alignedSymptom -= options.pipelineLatency;
    }
    for (const std::string& key : candidateSignals) {
        const SignalInfo* signal = ResolveSignal(source, key, error);
        if (!signal) return false;
        std::string initial;
        std::vector<Transition> transitions;
        if (!ReadWindow(source, *signal, start, end, transitions, initial, error)) return false;
        RootCauseEvidenceObservation observation;
        observation.signalName = key;
        observation.captureId = options.captureId;
        observation.analysisId = options.analysisId;
        observation.mappingAvailable = true;
        observation.dataComplete = true;
        observation.timeAligned = options.latencyKnown;
        observation.protocolError = interfaceAnomaly;
        if (interfaceAnomaly) observation.supportingFacts.push_back(
            "valid remained asserted while ready was low at the symptom");
        if (resetWasReleased) {
            observation.supportingFacts.push_back(
                "reset released at t=" + std::to_string(resetRelease));
        }
        TimeValue firstChange = std::numeric_limits<TimeValue>::max();
        std::string previous = initial;
        for (const auto& transition : transitions) {
            if (transition.value == previous) continue;
            firstChange = transition.time;
            break;
        }
        if (firstChange != std::numeric_limits<TimeValue>::max()) {
            observation.waveformChanged = true;
            if (firstChange <= alignedSymptom) {
                observation.supportingFacts.push_back(
                    "first candidate change at t=" + std::to_string(firstChange) +
                    " before the aligned symptom");
            } else {
                observation.missingConditions.push_back(
                    "candidate change occurs after the aligned symptom");
            }
        } else {
            observation.missingConditions.push_back(
                "no candidate transition in the bounded evidence window");
        }
        if (!options.latencyKnown) {
            observation.missingConditions.push_back(
                "pipeline latency is unknown; temporal ordering is downgraded");
        }
        if (!observation.waveformChanged && !interfaceAnomaly && !resetWasReleased) {
            observation.state = EvidenceObservationState::Unknown;
            observation.missingConditions.push_back("no positive dynamic evidence was observed");
        } else {
            observation.state = EvidenceObservationState::Observed;
        }
        out.push_back(std::move(observation));
    }
    return true;
}

} // namespace sigflow::debug
