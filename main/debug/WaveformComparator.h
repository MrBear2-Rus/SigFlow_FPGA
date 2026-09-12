#pragma once

#include "WaveformAligner.h"
#include "RootCauseGraph.h"

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "vcd.h"
}

namespace sigflow {
namespace debug {

// 单个差异点。
struct WaveformDiff {
    std::string signalName;   // 信号名
    timestamp_t hwTime = 0;   // HW VCD 中的时间
    timestamp_t simTime = 0;  // Sim VCD 中的时间
    std::string expectedValue; // 仿真期望值
    std::string actualValue;   // 硬件实测值
    double confidence = 0.0;   // 置信度 0..1
    std::size_t upstreamRank = 0;
    SignalLocation location;
    std::vector<RootCauseCandidate> upstreamCandidates;
};

// 比较结果。
struct ComparisonResult {
    bool aligned = false;
    AlignmentResult alignment;
    std::vector<WaveformDiff> diffs;        // 所有差异
    std::vector<WaveformDiff> firstDiffs;   // 每个信号的首个差异
    std::string summary;                    // 人可读总结
    std::size_t totalSignalsCompared = 0;
    std::size_t totalDiffs = 0;
    std::string referenceKind = "simulation";
    bool stimulusVerified = false;
    double stimulusScore = 0.0;
};

// 波形比较器：对齐后扫描首个差异。
class WaveformComparator {
public:
    // 比较两个 VCD，使用指定对齐结果。
    static ComparisonResult Compare(vcd_t* hw, vcd_t* sim,
                                    const AlignmentResult& alignment);

    // 简化版：自动对齐 + 比较。
    static ComparisonResult CompareAuto(vcd_t* hw, vcd_t* sim,
                                        std::uint16_t triggerIndex = 0);

    static void Enrich(ComparisonResult& result,
                       const SignalSourceMap& sourceMap,
                       const DependencyGraph& graph,
                       std::size_t maxDepth = 4,
                       std::size_t maxCandidates = 8);

private:
    // 取信号在指定时间的值（字符串）。
    static std::string GetValueAt(vcd_t* vcd, const signal_t* sig, timestamp_t t);
    // 在两个 VCD 中找共有信号名列表。
    static std::vector<std::pair<signal_t*, signal_t*>> FindCommonSignals(vcd_t* hw, vcd_t* sim);
};

} // namespace debug
} // namespace sigflow
