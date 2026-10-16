#include "WavePatternSearch.h"

#include <algorithm>
#include <set>

namespace sigflow {
namespace wave {

namespace {

bool EvaluateCriterion(sigflow::trace::TraceSource& source,
                       const WavePatternCriterion& criterion,
                       sigflow::trace::TimeValue t, std::string& error)
{
    const sigflow::trace::SignalInfo* signal = source.SignalById(criterion.signalId);
    if (!signal) {
        error = "unknown signal id in pattern";
        return false;
    }
    if (criterion.kind == WavePatternKind::DontCare) return true;

    std::string valueNow;
    if (!source.ValueAt(*signal, t, valueNow, error)) return false;
    if (criterion.kind == WavePatternKind::High) return valueNow == "1";
    if (criterion.kind == WavePatternKind::Low) return valueNow == "0";
    if (criterion.kind == WavePatternKind::Value) return valueNow == criterion.value;

    // 边沿：需要与 t-1 时刻的值比较
    std::string valueBefore;
    if (t == 0) {
        valueBefore = valueNow;
    } else if (!source.ValueAt(*signal, t - 1, valueBefore, error)) {
        return false;
    }
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
    for (const WavePatternCriterion& criterion : spec.criteria) {
        if (options.shouldCancel && options.shouldCancel()) {
            error = "pattern search cancelled";
            return results;
        }
        const sigflow::trace::SignalInfo* signal = source.SignalById(criterion.signalId);
        if (!signal) {
            error = "unknown signal id in pattern";
            return results;
        }
        std::vector<sigflow::trace::Transition> transitions;
        if (!source.Query(*signal, t0, t1, transitions, error)) return results;
        for (const sigflow::trace::Transition& tr : transitions) {
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
            if (!EvaluateCriterion(source, criterion, t, error)) {
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
