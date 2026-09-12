#include "WaveformComparator.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sigflow {
namespace debug {
namespace {

std::string NormalizeValue(const char* raw) {
    if (!raw) return {};
    std::string value(raw);
    if (!value.empty() && (value.front() == 'b' || value.front() == 'B')) {
        value.erase(value.begin());
    }
    return value;
}

signal_t* FindSignal(vcd_t* vcd, const std::string& name) {
    if (!vcd || name.empty()) return nullptr;
    for (std::size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* signal = &vcd->signals[i];
        if (name == signal->name || name == signal->full_name) return signal;
    }
    for (std::size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* signal = &vcd->signals[i];
        const std::string fullName = signal->full_name;
        if (fullName.size() <= name.size() ||
            fullName.compare(fullName.size() - name.size(), name.size(), name) != 0 ||
            fullName[fullName.size() - name.size() - 1] != '.') {
            continue;
        }
        return signal;
    }
    return nullptr;
}

bool MapHwToSim(timestamp_t hwTime, std::int64_t offset,
                timestamp_t simMax, timestamp_t& simTime) {
    const std::int64_t mapped = static_cast<std::int64_t>(hwTime) + offset;
    if (mapped < 0 || mapped > static_cast<std::int64_t>(simMax)) return false;
    simTime = static_cast<timestamp_t>(mapped);
    return true;
}

bool MapSimToHw(timestamp_t simTime, std::int64_t offset,
                timestamp_t hwMax, timestamp_t& hwTime) {
    const std::int64_t mapped = static_cast<std::int64_t>(simTime) - offset;
    if (mapped < 0 || mapped > static_cast<std::int64_t>(hwMax)) return false;
    hwTime = static_cast<timestamp_t>(mapped);
    return true;
}

} // namespace

std::string WaveformComparator::GetValueAt(vcd_t*, const signal_t* signal, timestamp_t t) {
    if (!signal || signal->changes_count == 0 ||
        signal->value_changes[0].timestamp > t) return {};
    std::size_t low = 0;
    std::size_t high = signal->changes_count;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (signal->value_changes[middle].timestamp <= t) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NormalizeValue(signal->value_changes[low - 1].value);
}

std::vector<std::pair<signal_t*, signal_t*>> WaveformComparator::FindCommonSignals(
    vcd_t* hw, vcd_t* sim) {
    std::vector<std::pair<signal_t*, signal_t*>> result;
    if (!hw || !sim) return result;
    for (std::size_t i = 0; i < hw->signals_count; ++i) {
        signal_t* hwSignal = &hw->signals[i];
        signal_t* simSignal = FindSignal(sim, hwSignal->full_name);
        if (!simSignal) simSignal = FindSignal(sim, hwSignal->name);
        if (simSignal) result.emplace_back(hwSignal, simSignal);
    }
    return result;
}

ComparisonResult WaveformComparator::Compare(vcd_t* hw, vcd_t* sim,
                                             const AlignmentResult& alignment) {
    ComparisonResult result;
    result.alignment = alignment;
    result.aligned = alignment.valid;
    if (!alignment.valid) {
        result.summary = "对齐失败：" + alignment.message;
        return result;
    }
    if (!hw || !sim) {
        result.aligned = false;
        result.summary = "VCD 数据为空";
        return result;
    }

    const auto common = FindCommonSignals(hw, sim);
    result.totalSignalsCompared = common.size();
    if (common.empty()) {
        result.aligned = false;
        result.summary = "未找到共有信号（可能探针名与仿真信号名不一致）";
        return result;
    }

    const timestamp_t hwMax = vcd_get_max_timestamp(hw);
    const timestamp_t simMax = vcd_get_max_timestamp(sim);
    const std::int64_t simEndInHw = static_cast<std::int64_t>(simMax) -
                                    alignment.timeOffset;
    if (simEndInHw < 0) {
        result.summary = "对齐后两份 VCD 没有重叠时间窗口";
        return result;
    }
    const timestamp_t scanStart = 0;
    const timestamp_t scanEnd = std::min(
        hwMax, static_cast<timestamp_t>(simEndInHw));

    for (const auto& [hwSignal, simSignal] : common) {
        std::set<timestamp_t> sampleTimes;
        sampleTimes.insert(alignment.hwAnchorTime);
        for (std::size_t i = 0; i < hwSignal->changes_count; ++i) {
            const timestamp_t time = hwSignal->value_changes[i].timestamp;
            if (time >= scanStart && time <= scanEnd) sampleTimes.insert(time);
        }
        for (std::size_t i = 0; i < simSignal->changes_count; ++i) {
            timestamp_t mapped = 0;
            if (MapSimToHw(simSignal->value_changes[i].timestamp,
                           alignment.timeOffset, hwMax, mapped) &&
                mapped >= scanStart && mapped <= scanEnd) {
                sampleTimes.insert(mapped);
            }
        }

        for (const timestamp_t hwTime : sampleTimes) {
            if (hwTime < scanStart || hwTime > scanEnd) continue;
            timestamp_t simTime = 0;
            if (!MapHwToSim(hwTime, alignment.timeOffset, simMax, simTime)) continue;
            const std::string actual = GetValueAt(hw, hwSignal, hwTime);
            const std::string expected = GetValueAt(sim, simSignal, simTime);
            if (actual.empty() || expected.empty() || actual == expected) continue;

            WaveformDiff diff;
            diff.signalName = hwSignal->name;
            diff.hwTime = hwTime;
            diff.simTime = simTime;
            diff.expectedValue = expected;
            diff.actualValue = actual;
            diff.confidence = alignment.qualityScore;
            result.diffs.push_back(std::move(diff));
        }
    }

    std::sort(result.diffs.begin(), result.diffs.end(),
              [](const WaveformDiff& left, const WaveformDiff& right) {
                  if (left.hwTime != right.hwTime) return left.hwTime < right.hwTime;
                  if (left.upstreamRank != right.upstreamRank) {
                      return left.upstreamRank < right.upstreamRank;
                  }
                  return left.signalName < right.signalName;
              });
    std::set<std::string> seenSignals;
    for (const auto& diff : result.diffs) {
        if (seenSignals.insert(diff.signalName).second) result.firstDiffs.push_back(diff);
    }
    std::sort(result.firstDiffs.begin(), result.firstDiffs.end(),
              [](const WaveformDiff& left, const WaveformDiff& right) {
                  if (left.hwTime != right.hwTime) return left.hwTime < right.hwTime;
                  if (left.upstreamRank != right.upstreamRank) {
                      return left.upstreamRank < right.upstreamRank;
                  }
                  return left.signalName < right.signalName;
              });
    result.totalDiffs = result.diffs.size();

    std::ostringstream summary;
    summary << "对齐方式：" << alignment.message << "\n"
            << "对齐质量：" << static_cast<int>(alignment.qualityScore * 100.0) << "%";
    if (alignment.needsManualReview) summary << "（待人工确认）";
    summary << "\n比较信号数：" << result.totalSignalsCompared
            << "\n差异总数：" << result.totalDiffs;
    if (!result.firstDiffs.empty()) {
        const auto& first = result.firstDiffs.front();
        summary << "\n首个差异：信号[" << first.signalName
                << "] HW t=" << first.hwTime
                << " 期望[" << first.expectedValue
                << "] 实测[" << first.actualValue << "]";
    } else {
        summary << "\n所有共有信号在重叠窗口内一致";
    }
    if (!alignment.qualityMessage.empty()) {
        summary << "\n对齐说明：" << alignment.qualityMessage;
    }
    result.summary = summary.str();
    return result;
}

ComparisonResult WaveformComparator::CompareAuto(vcd_t* hw, vcd_t* sim,
                                                std::uint16_t triggerIndex) {
    return Compare(hw, sim, WaveformAligner::AlignAuto(hw, sim, triggerIndex));
}

void WaveformComparator::Enrich(ComparisonResult& result,
                                const SignalSourceMap& sourceMap,
                                const DependencyGraph& graph,
                                std::size_t maxDepth,
                                std::size_t maxCandidates) {
    auto enrich = [&](WaveformDiff& diff) {
        const SignalLocation* location = sourceMap.Find(diff.signalName);
        if (location) diff.location = *location;
        diff.upstreamCandidates = RootCauseAnalyzer::Rank(
            diff.signalName, graph, sourceMap, maxDepth, maxCandidates);
        if (!diff.upstreamCandidates.empty()) diff.upstreamRank = 1;
    };
    for (auto& diff : result.diffs) enrich(diff);
    for (auto& diff : result.firstDiffs) enrich(diff);
    std::sort(result.diffs.begin(), result.diffs.end(),
              [](const WaveformDiff& left, const WaveformDiff& right) {
                  if (left.hwTime != right.hwTime) return left.hwTime < right.hwTime;
                  if (left.upstreamRank != right.upstreamRank) {
                      return left.upstreamRank < right.upstreamRank;
                  }
                  return left.signalName < right.signalName;
              });
    std::sort(result.firstDiffs.begin(), result.firstDiffs.end(),
              [](const WaveformDiff& left, const WaveformDiff& right) {
                  if (left.hwTime != right.hwTime) return left.hwTime < right.hwTime;
                  if (left.upstreamRank != right.upstreamRank) {
                      return left.upstreamRank < right.upstreamRank;
                  }
                  return left.signalName < right.signalName;
              });
}

} // namespace debug
} // namespace sigflow
