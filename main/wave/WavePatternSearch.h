#pragma once

#include "../trace/TraceSource.h"

#include <cstddef>
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

// 多信号同时满足条件的搜索（W3-04，对标 Bear2Wave pattern_search 语义）。
// 候选时刻取所有相关信号的跳变时刻并集，在 [t0, t1] 内按序/逆序评估。
bool WavePatternFind(sigflow::trace::TraceSource& source,
                     const WavePatternSpec& spec,
                     sigflow::trace::TimeValue from, sigflow::trace::TimeValue to,
                     bool forward, int repeat, sigflow::trace::TimeValue& outTime,
                     std::string& error);

std::vector<sigflow::trace::TimeValue>
WavePatternCollect(sigflow::trace::TraceSource& source,
                   const WavePatternSpec& spec,
                   sigflow::trace::TimeValue t0, sigflow::trace::TimeValue t1,
                   std::size_t maxMatches, std::string& error);

} // namespace wave
} // namespace sigflow
