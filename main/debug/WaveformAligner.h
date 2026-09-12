#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "vcd.h"
}

namespace sigflow {
namespace debug {

// 对齐锚点类型（三层锚点，按优先级递降）。
enum class AnchorKind {
    ResetRelease,   // 一级：复位释放边沿
    TriggerHit,     // 二级：触发命中样本
    InputTxn,       // 三级：顶层输入事务变化序列
};

// 对齐结果。
struct AlignmentResult {
    bool valid = false;
    int64_t timeOffset = 0;       // sim_time - hw_time（hw_time + offset ≈ sim_time）
    AnchorKind anchorKind = AnchorKind::ResetRelease;
    std::string anchorSignal;    // 用作锚点的信号名
    timestamp_t hwAnchorTime = 0; // HW VCD 中锚点时间
    timestamp_t simAnchorTime = 0;// Sim VCD 中锚点时间
    double qualityScore = 0.0;     // 对齐质量 0..1
    bool needsManualReview = true;
    std::size_t matchedAnchors = 0;
    std::size_t transactionMatches = 0;
    std::size_t transactionCompared = 0;
    std::string qualityMessage;
    std::string message;
};

// 波形对齐器：将 HW VCD 与 Sim VCD 的时间轴对齐。
class WaveformAligner {
public:
    // 用信号名列表尝试找共同锚点。
    // anchorCandidates 按优先级排列（如 ["rst_n", "reset", "trigger"]）。
    // triggerIndex：HW 硬件捕获的触发样本索引（对应 hwAnchorTime = triggerIndex）。
    static AlignmentResult Align(vcd_t* hw, vcd_t* sim,
                                 const std::vector<std::string>& anchorCandidates,
                                 std::uint16_t triggerIndex = 0);

    // 简化版：自动搜索首个在两个 VCD 中都存在的信号的首个变化沿作为锚点。
    static AlignmentResult AlignAuto(vcd_t* hw, vcd_t* sim,
                                     std::uint16_t triggerIndex = 0);

private:
    // 在 VCD 中找指定信号的首个变化时间戳。
    static timestamp_t FindFirstChange(vcd_t* vcd, const std::string& signalName);
    // 在 VCD 中找所有信号中首 个变化时间戳（全局最早）。
    static timestamp_t FindEarliestChange(vcd_t* vcd);
    // 在信号变化序列中找指定值出现的首个时间戳。
    static timestamp_t FindValueChange(vcd_t* vcd, const std::string& signalName,
                                       const std::string& targetValue);
};

} // namespace debug
} // namespace sigflow
