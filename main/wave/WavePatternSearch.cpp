#include "WavePatternSearch.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace sigflow {
namespace wave {

namespace {

struct PatternSignalState {
    const sigflow::trace::SignalInfo* signal = nullptr;
    sigflow::trace::TimeValue begin = 0;
    std::vector<sigflow::trace::Transition> transitions;
    std::string initialValue;
    std::string beforeBeginValue;
    bool hasBeforeBeginValue = false;
};

std::string CachedValueAt(const PatternSignalState& state,
                          sigflow::trace::TimeValue time)
{
    if (time < state.begin) {
        return state.hasBeforeBeginValue ? state.beforeBeginValue : state.initialValue;
    }
    const auto it = std::upper_bound(
        state.transitions.begin(), state.transitions.end(), time,
        [](sigflow::trace::TimeValue value, const sigflow::trace::Transition& transition) {
            return value < transition.time;
        });
    if (it == state.transitions.begin()) return state.initialValue;
    return (it - 1)->value;
}

bool EvaluateCriterionCached(const PatternSignalState& state,
                             const WavePatternCriterion& criterion,
                             sigflow::trace::TimeValue t)
{
    if (criterion.kind == WavePatternKind::DontCare) return true;

    const std::string valueNow = CachedValueAt(state, t);
    if (criterion.kind == WavePatternKind::High) return valueNow == "1";
    if (criterion.kind == WavePatternKind::Low) return valueNow == "0";
    if (criterion.kind == WavePatternKind::Value) return valueNow == criterion.value;

    // 边沿：需要与 t-1 时刻的值比较
    const std::string valueBefore = t == 0 ? valueNow : CachedValueAt(state, t - 1);
    if (criterion.kind == WavePatternKind::RisingEdge) {
        return valueBefore != "1" && valueNow == "1";
    }
    if (criterion.kind == WavePatternKind::FallingEdge) {
        return valueBefore == "1" && valueNow != "1";
    }
    return valueBefore != valueNow; // AnyEdge
}

} // namespace

std::vector<sigflow::trace::TimeValue>
WavePatternCollect(sigflow::trace::TraceSource& source,
                   const WavePatternSpec& spec,
                   sigflow::trace::TimeValue t0, sigflow::trace::TimeValue t1,
                   std::size_t maxMatches, std::string& error)
{
    WavePatternSearchOptions options;
    // Preserve the legacy API's unlimited candidate behavior. New callers
    // should use the options overload so they can keep a bounded UI search.
    options.maxCandidates = 0;
    options.maxMatches = maxMatches;
    return WavePatternCollect(source, spec, t0, t1, options, error);
}

std::vector<sigflow::trace::TimeValue>
WavePatternCollect(sigflow::trace::TraceSource& source,
                   const WavePatternSpec& spec,
                   sigflow::trace::TimeValue t0, sigflow::trace::TimeValue t1,
                   const WavePatternSearchOptions& options, std::string& error)
{
    error.clear();
    std::vector<sigflow::trace::TimeValue> results;
    if (spec.criteria.empty() || t1 < t0) return results;

    // 候选时刻 = 相关信号跳变时刻并集
    std::set<sigflow::trace::TimeValue> candidates;
    std::unordered_map<int, PatternSignalState> signalStates;
    for (const WavePatternCriterion& criterion : spec.criteria) {
        if (options.shouldCancel && options.shouldCancel()) {
            error = "pattern search cancelled";
            return results;
        }
        auto stateIt = signalStates.find(criterion.signalId);
        if (stateIt == signalStates.end()) {
            const sigflow::trace::SignalInfo* signal = source.SignalById(criterion.signalId);
            if (!signal) {
                error = "unknown signal id in pattern";
                return results;
            }
            PatternSignalState state;
            state.signal = signal;
            state.begin = t0;
            if (!source.ValueAt(*signal, t0, state.initialValue, error)) return results;
            if (state.initialValue.empty()) state.initialValue = "x";
            if (t0 > 0 && !source.ValueAt(*signal, t0 - 1, state.beforeBeginValue, error)) {
                return results;
            }
            state.hasBeforeBeginValue = t0 > 0;
            if (!source.Query(*signal, t0, t1, state.transitions, error)) return results;
            stateIt = signalStates.emplace(criterion.signalId, std::move(state)).first;
        }
        for (const sigflow::trace::Transition& tr : stateIt->second.transitions) {
            if (options.shouldCancel && options.shouldCancel()) {
                error = "pattern search cancelled";
                return results;
            }
            candidates.insert(tr.time);
            if (options.maxCandidates > 0 && candidates.size() > options.maxCandidates) {
                error = "pattern search candidate limit exceeded";
                results.clear();
                return results;
            }
        }
    }

    for (sigflow::trace::TimeValue t : candidates) {
        if (options.shouldCancel && options.shouldCancel()) {
            error = "pattern search cancelled";
            results.clear();
            return results;
        }
        if (t < t0 || t > t1) continue;
        bool allMatch = true;
        for (const WavePatternCriterion& criterion : spec.criteria) {
            const auto stateIt = signalStates.find(criterion.signalId);
            if (stateIt == signalStates.end() ||
                !EvaluateCriterionCached(stateIt->second, criterion, t)) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) {
            results.push_back(t);
            if (options.maxMatches > 0 && results.size() >= options.maxMatches) break;
        }
    }
    return results;
}

bool WavePatternFind(sigflow::trace::TraceSource& source,
                     const WavePatternSpec& spec,
                     sigflow::trace::TimeValue from, sigflow::trace::TimeValue to,
                     bool forward, int repeat, sigflow::trace::TimeValue& outTime,
                     std::string& error)
{
    WavePatternSearchOptions options;
    options.maxCandidates = 0;
    return WavePatternFind(source, spec, from, to, forward, repeat,
                           options, outTime, error);
}

bool WavePatternFind(sigflow::trace::TraceSource& source,
                     const WavePatternSpec& spec,
                     sigflow::trace::TimeValue from, sigflow::trace::TimeValue to,
                     bool forward, int repeat,
                     const WavePatternSearchOptions& options,
                     sigflow::trace::TimeValue& outTime,
                     std::string& error)
{
    if (repeat < 1) repeat = 1;
    std::vector<sigflow::trace::TimeValue> matches =
        WavePatternCollect(source, spec, from, to, options, error);
    if (matches.empty()) return false;
    if (!forward) {
        std::reverse(matches.begin(), matches.end());
    }
    const std::size_t index = static_cast<std::size_t>(repeat - 1);
    if (index >= matches.size()) return false;
    outTime = matches[index];
    return true;
}

} // namespace wave
} // namespace sigflow
