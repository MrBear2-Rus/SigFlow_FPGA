#pragma once

#include "../trace/TraceSource.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace sigflow {
namespace wave {

enum class WavePatternKind {
    DontCare,
    RisingEdge,
    FallingEdge,
    AnyEdge,
    High,
    Low,
    Value,
};

struct WavePatternCriterion {
    int signalId = -1;
    WavePatternKind kind = WavePatternKind::DontCare;
    std::string value; // Value 时比较的数值串
};

struct WavePatternSpec {
    std::vector<WavePatternCriterion> criteria;
};

// Bounded/cancellable search controls. A zero candidate limit means
// unlimited; callers that run on the UI thread should keep the default bound.
struct WavePatternSearchOptions {
    std::size_t maxCandidates = 250000;
    std::size_t maxMatches = 0;
    std::function<bool()> shouldCancel;
};

// 多信号同时满足条件的搜索（W3-04，对标 Bear2Wave pattern_search 语义）。
// 候选时刻取所有相关信号的跳变时刻并集，在 [t0, t1] 内按序/逆序评估。
bool WavePatternFind(sigflow::trace::TraceSource& source,
                     const WavePatternSpec& spec,
                     sigflow::trace::TimeValue from, sigflow::trace::TimeValue to,
                     bool forward, int repeat, sigflow::trace::TimeValue& outTime,
                     std::string& error);

bool WavePatternFind(sigflow::trace::TraceSource& source,
                     const WavePatternSpec& spec,
                     sigflow::trace::TimeValue from, sigflow::trace::TimeValue to,
                     bool forward, int repeat,
                     const WavePatternSearchOptions& options,
                     sigflow::trace::TimeValue& outTime,
                     std::string& error);

std::vector<sigflow::trace::TimeValue>
WavePatternCollect(sigflow::trace::TraceSource& source,
                   const WavePatternSpec& spec,
                   sigflow::trace::TimeValue t0, sigflow::trace::TimeValue t1,
                   std::size_t maxMatches, std::string& error);

std::vector<sigflow::trace::TimeValue>
WavePatternCollect(sigflow::trace::TraceSource& source,
                   const WavePatternSpec& spec,
                   sigflow::trace::TimeValue t0, sigflow::trace::TimeValue t1,
                   const WavePatternSearchOptions& options,
                   std::string& error);

} // namespace wave
} // namespace sigflow
