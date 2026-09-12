#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// 采集探针（由 DebugContract 探针派生）。
struct CaptureProbe {
    std::string id;        // 契约探针 id
    std::string path;      // 层级路径（如 top.state）
    unsigned width = 1;
    unsigned bitOffset = 0;
};

// 解码元信息：触发样本在 VCD 时间轴上的位置（显示层将其作为时间零点）。
struct CaptureVcdMeta {
    std::uint64_t triggerTime = 0;
    std::string timescale = "1ns";
};

// 将环形采集样本解码为 VCD（T-P3-03）。
// 时间轴 = 环形地址序（0..depth-1）：相对触发时刻 k 的样本位于
// samples[(triggerIndex + k) % depth]，因此环形地址序即相对时间序，
// 触发样本位于时间 triggerIndex（见 meta.triggerTime）。
// 输出与 WavePanel 的 VcdLazyTraceSource 兼容（$timescale/$scope/$var/值变更）。
bool DecodeCaptureToVcd(const std::vector<std::uint32_t>& samples,
                        std::uint32_t depth, std::uint16_t triggerIndex,
                        const std::vector<CaptureProbe>& probes,
                        const std::string& path, CaptureVcdMeta& meta,
                        std::string& error);

// capture.raw 读取（与 SaveCaptureRaw 配套，格式见 DebugProtocol.h）。
bool LoadCaptureRaw(const std::string& path, std::uint32_t& depth, std::uint32_t& width,
                    std::uint16_t& start, std::vector<std::uint32_t>& samples,
                    std::string& error);

} // namespace debug
} // namespace sigflow
