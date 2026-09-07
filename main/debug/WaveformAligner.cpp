#include "WaveformAligner.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace sigflow {
namespace debug {
namespace {

struct SignalPair {
    signal_t* hw = nullptr;
    signal_t* sim = nullptr;
};

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

std::string LeafName(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

std::string NormalizeValue(const char* raw) {
    if (!raw) return {};
    std::string value(raw);
    if (!value.empty() && (value.front() == 'b' || value.front() == 'B')) {
        value.erase(value.begin());
    }
    return value;
}

bool IsSingleValue(const char* raw, char expected) {
    const std::string value = NormalizeValue(raw);
    return value.size() == 1 &&
           static_cast<char>(std::tolower(static_cast<unsigned char>(value[0]))) == expected;
}

bool IsResetName(const std::string& name) {
    const std::string leaf = LeafName(name);
    std::string lower;
    lower.reserve(leaf.size());
    for (const char character : leaf) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return lower == "rst" || lower == "reset" || lower == "arst" ||
           lower == "rst_n" || lower == "reset_n" || lower == "arst_n" ||
           lower.find("reset") != std::string::npos;
}

bool IsActiveLowReset(const std::string& name) {
    const std::string leaf = LeafName(name);
    return !leaf.empty() && (leaf.back() == 'n' || leaf.ends_with("_b"));
}

bool IsTriggerName(const std::string& name) {
    std::string lower = LeafName(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("trigger") != std::string::npos || lower == "fire" ||
           lower == "valid" || lower == "hs_valid";
}

bool FindEdge(vcd_t* vcd, const std::string& name, char from, char to,
              timestamp_t& timestamp) {
    signal_t* signal = FindSignal(vcd, name);
    if (!signal || signal->changes_count == 0) return false;
    bool havePrevious = false;
    char previous = 0;
    for (std::size_t i = 0; i < signal->changes_count; ++i) {
        const std::string value = NormalizeValue(signal->value_changes[i].value);
        if (value.size() != 1) {
            havePrevious = false;
            continue;
        }
        const char current = static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
        if (havePrevious && previous == from && current == to) {
            timestamp = signal->value_changes[i].timestamp;
            return true;
        }
        previous = current;
        havePrevious = true;
    }
    return false;
}

bool FindFirstValue(vcd_t* vcd, const std::string& name, char expected,
                    timestamp_t& timestamp) {
    signal_t* signal = FindSignal(vcd, name);
    if (!signal) return false;
    for (std::size_t i = 0; i < signal->changes_count; ++i) {
        if (IsSingleValue(signal->value_changes[i].value, expected)) {
            timestamp = signal->value_changes[i].timestamp;
            return true;
        }
    }
    return false;
}

bool FindFirstChangeInternal(vcd_t* vcd, const std::string& name, timestamp_t& timestamp) {
    signal_t* signal = FindSignal(vcd, name);
    if (!signal || signal->changes_count == 0) return false;
    timestamp = signal->value_changes[0].timestamp;
    return true;
}

bool FindEarliestChangeInternal(vcd_t* vcd, timestamp_t& timestamp) {
    if (!vcd) return false;
    bool found = false;
    timestamp_t earliest = std::numeric_limits<timestamp_t>::max();
    for (std::size_t i = 0; i < vcd->signals_count; ++i) {
        const signal_t& signal = vcd->signals[i];
        if (signal.changes_count == 0) continue;
        if (!found || signal.value_changes[0].timestamp < earliest) {
            earliest = signal.value_changes[0].timestamp;
            found = true;
        }
    }
    if (found) timestamp = earliest;
    return found;
}

std::vector<SignalPair> FindCommonSignals(vcd_t* hw, vcd_t* sim) {
    std::vector<SignalPair> result;
    if (!hw || !sim) return result;
    for (std::size_t i = 0; i < hw->signals_count; ++i) {
        signal_t* hwSignal = &hw->signals[i];
        signal_t* simSignal = FindSignal(sim, hwSignal->full_name);
        if (!simSignal) simSignal = FindSignal(sim, hwSignal->name);
        if (simSignal) result.push_back({hwSignal, simSignal});
    }
    return result;
}

bool InferInputTransaction(vcd_t* hw, vcd_t* sim, timestamp_t& hwAnchor,
                           timestamp_t& simAnchor, std::int64_t& offset,
                           std::size_t& matches, std::size_t& compared,
                           std::string& signalName) {
    const auto common = FindCommonSignals(hw, sim);
    std::vector<std::int64_t> candidateOffsets;
    struct Match {
        timestamp_t hwTime;
        timestamp_t simTime;
        std::int64_t offset;
    };
    std::vector<Match> allMatches;

    for (const auto& pair : common) {
        const std::string name = pair.hw->name;
        if (IsResetName(name) || IsTriggerName(name)) continue;
        const std::size_t limit = std::min<std::size_t>(
            std::min(pair.hw->changes_count, pair.sim->changes_count), 32);
        compared += limit;
        for (std::size_t i = 0; i < limit; ++i) {
            const std::string hwValue = NormalizeValue(pair.hw->value_changes[i].value);
            const std::string simValue = NormalizeValue(pair.sim->value_changes[i].value);
            if (hwValue != simValue) continue;
            const auto hwTime = pair.hw->value_changes[i].timestamp;
            const auto simTime = pair.sim->value_changes[i].timestamp;
            const auto candidate = static_cast<std::int64_t>(simTime) -
                                   static_cast<std::int64_t>(hwTime);
            candidateOffsets.push_back(candidate);
            allMatches.push_back({hwTime, simTime, candidate});
        }
    }
    if (candidateOffsets.empty()) return false;

    std::int64_t bestOffset = candidateOffsets.front();
    std::size_t bestCount = 0;
    for (const std::int64_t candidate : candidateOffsets) {
        std::size_t count = 0;
        for (const std::int64_t other : candidateOffsets) {
            if (std::llabs(candidate - other) <= 1) ++count;
        }
        if (count > bestCount) {
            bestCount = count;
            bestOffset = candidate;
        }
    }
    if (bestCount < 2) return false;

    bool foundAnchor = false;
    for (const Match& match : allMatches) {
        if (std::llabs(match.offset - bestOffset) > 1) continue;
        ++matches;
        if (!foundAnchor || match.hwTime < hwAnchor) {
            hwAnchor = match.hwTime;
            simAnchor = match.simTime;
            foundAnchor = true;
        }
    }
    if (!foundAnchor) return false;
    offset = bestOffset;
    signalName = "input_transaction";
    return true;
}

void SetQuality(AlignmentResult& result, double score, bool manual,
                const std::string& qualityMessage) {
    result.qualityScore = std::clamp(score, 0.0, 1.0);
    result.needsManualReview = manual;
    result.qualityMessage = qualityMessage;
}

} // namespace

timestamp_t WaveformAligner::FindFirstChange(vcd_t* vcd, const std::string& signalName) {
    timestamp_t timestamp = 0;
    return FindFirstChangeInternal(vcd, signalName, timestamp) ? timestamp : 0;
}

timestamp_t WaveformAligner::FindEarliestChange(vcd_t* vcd) {
    timestamp_t timestamp = 0;
    return FindEarliestChangeInternal(vcd, timestamp) ? timestamp : 0;
}

timestamp_t WaveformAligner::FindValueChange(vcd_t* vcd, const std::string& signalName,
                                             const std::string& targetValue) {
    signal_t* signal = FindSignal(vcd, signalName);
    if (!signal) return 0;
    const std::string expected = NormalizeValue(targetValue.c_str());
    for (std::size_t i = 0; i < signal->changes_count; ++i) {
        if (NormalizeValue(signal->value_changes[i].value) == expected) {
            return signal->value_changes[i].timestamp;
        }
    }
    return 0;
}

AlignmentResult WaveformAligner::Align(vcd_t* hw, vcd_t* sim,
                                       const std::vector<std::string>& anchorCandidates,
                                       std::uint16_t triggerIndex) {
    AlignmentResult result;
    if (!hw || !sim) {
        result.message = "VCD 数据为空";
        result.qualityMessage = "缺少硬件或仿真波形";
        return result;
    }

    for (const auto& name : anchorCandidates) {
        if (!IsResetName(name)) continue;
        timestamp_t hwTime = 0;
        timestamp_t simTime = 0;
        const char from = IsActiveLowReset(name) ? '0' : '1';
        const char to = IsActiveLowReset(name) ? '1' : '0';
        if (!FindEdge(hw, name, from, to, hwTime) ||
            !FindEdge(sim, name, from, to, simTime)) continue;
        result.valid = true;
        result.anchorKind = AnchorKind::ResetRelease;
        result.anchorSignal = name;
        result.hwAnchorTime = hwTime;
        result.simAnchorTime = simTime;
        result.timeOffset = static_cast<std::int64_t>(simTime) -
                            static_cast<std::int64_t>(hwTime);
        result.matchedAnchors = 1;
        SetQuality(result, 0.98, false, "检测到 HW/Sim 相同复位释放边沿");
        result.message = "复位释放锚点[" + name + "]：HW t=" +
                         std::to_string(hwTime) + "，Sim t=" + std::to_string(simTime) +
                         "，偏移=" + std::to_string(result.timeOffset);
        return result;
    }

    for (const auto& name : anchorCandidates) {
        if (IsResetName(name) || IsTriggerName(name)) continue;
        timestamp_t hwTime = 0;
        timestamp_t simTime = 0;
        if (!FindFirstChangeInternal(hw, name, hwTime) ||
            !FindFirstChangeInternal(sim, name, simTime)) continue;
        result.valid = true;
        result.anchorKind = AnchorKind::ResetRelease;
        result.anchorSignal = name;
        result.hwAnchorTime = hwTime;
        result.simAnchorTime = simTime;
        result.timeOffset = static_cast<std::int64_t>(simTime) -
                            static_cast<std::int64_t>(hwTime);
        result.matchedAnchors = 1;
        SetQuality(result, 0.84, false, "使用用户指定的同步标志首个样本");
        result.message = "用户同步标志[" + name + "]：HW t=" +
                         std::to_string(hwTime) + "，Sim t=" + std::to_string(simTime) +
                         "，偏移=" + std::to_string(result.timeOffset);
        return result;
    }

    const std::vector<std::string> triggerNames = {
        "trigger", "trigger_hit", "capture_trigger", "valid", "hs_valid", "fire"};
    for (const auto& name : triggerNames) {
        timestamp_t simTime = 0;
        if (!FindFirstValue(sim, name, '1', simTime) &&
            !FindEdge(sim, name, '0', '1', simTime)) continue;
        timestamp_t hwTime = 0;
        if (triggerIndex != 0) {
            hwTime = triggerIndex;
        } else if (!FindFirstValue(hw, name, '1', hwTime) &&
                   !FindEdge(hw, name, '0', '1', hwTime)) {
            continue;
        }
        result.valid = true;
        result.anchorKind = AnchorKind::TriggerHit;
        result.anchorSignal = name;
        result.hwAnchorTime = hwTime;
        result.simAnchorTime = simTime;
        result.timeOffset = static_cast<std::int64_t>(simTime) -
                            static_cast<std::int64_t>(hwTime);
        result.matchedAnchors = 1;
        SetQuality(result, triggerIndex == 0 ? 0.9 : 0.82,
                   triggerIndex != 0, triggerIndex == 0
                       ? "检测到 HW/Sim 相同触发信号"
                       : "硬件使用触发样本索引，仿真使用同名触发信号");
        result.message = "触发命中锚点[" + name + "]：HW t=" +
                         std::to_string(hwTime) + "，Sim t=" + std::to_string(simTime) +
                         "，偏移=" + std::to_string(result.timeOffset);
        return result;
    }

    if (triggerIndex != 0) {
        result.valid = true;
        result.anchorKind = AnchorKind::TriggerHit;
        result.anchorSignal = "trigger_index";
        result.hwAnchorTime = triggerIndex;
        result.simAnchorTime = triggerIndex;
        result.timeOffset = 0;
        result.matchedAnchors = 0;
        SetQuality(result, 0.45, true, "只有硬件触发样本索引，仿真没有同条件触发信号");
        result.message = "低置信度触发索引对齐：index=" + std::to_string(triggerIndex);
        return result;
    }

    timestamp_t hwTime = 0;
    timestamp_t simTime = 0;
    std::int64_t offset = 0;
    std::size_t matches = 0;
    std::size_t compared = 0;
    std::string signalName;
    if (InferInputTransaction(hw, sim, hwTime, simTime, offset, matches, compared,
                              signalName)) {
        result.valid = true;
        result.anchorKind = AnchorKind::InputTxn;
        result.anchorSignal = signalName;
        result.hwAnchorTime = hwTime;
        result.simAnchorTime = simTime;
        result.timeOffset = offset;
        result.matchedAnchors = matches;
        result.transactionMatches = matches;
        result.transactionCompared = compared;
        const double ratio = compared == 0 ? 0.0 :
            static_cast<double>(matches) / static_cast<double>(compared);
        SetQuality(result, 0.55 + std::min(0.35, ratio * 0.35), ratio < 0.75,
                   "通过共有输入信号的变化序列推断固定时间偏移");
        result.message = "输入事务锚点：匹配 " + std::to_string(matches) + "/" +
                         std::to_string(compared) + "，偏移=" + std::to_string(offset);
        return result;
    }

    if (!FindCommonSignals(hw, sim).empty() &&
        FindEarliestChangeInternal(hw, hwTime) &&
        FindEarliestChangeInternal(sim, simTime)) {
        result.valid = true;
        result.anchorKind = AnchorKind::InputTxn;
        result.anchorSignal = "earliest_change";
        result.hwAnchorTime = hwTime;
        result.simAnchorTime = simTime;
        result.timeOffset = static_cast<std::int64_t>(simTime) -
                            static_cast<std::int64_t>(hwTime);
        result.matchedAnchors = 0;
        SetQuality(result, 0.2, true, "只有全局最早变化可用，无法证明输入激励一致");
        result.message = "低置信度兜底对齐：HW t=" + std::to_string(hwTime) +
                         "，Sim t=" + std::to_string(simTime);
        return result;
    }

    result.message = "无法找到任何共同锚点或输入事务";
    result.qualityMessage = "两份 VCD 没有可匹配的时间锚点";
    return result;
}

AlignmentResult WaveformAligner::AlignAuto(vcd_t* hw, vcd_t* sim,
                                           std::uint16_t triggerIndex) {
    return Align(hw, sim, {"rst_n", "reset_n", "arst_n", "reset", "rst", "arst"},
                 triggerIndex);
}

} // namespace debug
} // namespace sigflow
