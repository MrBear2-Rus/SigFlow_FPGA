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

struct CaptureTimebase {
    std::uint64_t sampleClockHz = 0; // 0 = legacy sample-index time, not physical
    std::uint32_t decimation = 1;
    bool triggerObserved = true;
};

// All times are VCD ticks. Physical captures use 1ps and absolute rational
// conversion floor(i * decimation * 10^12 / sampleClockHz), without step drift.
struct CaptureVcdMeta {
    std::uint64_t triggerTime = 0;
    std::string timescale = "1ns";
    std::string timeDomain = "sample_index";
    std::uint64_t sampleClockHz = 0;
    std::uint32_t decimation = 1;
    std::uint64_t periodNumeratorPs = 0;
    std::uint64_t periodDenominator = 0;
    std::uint64_t lastSampleTime = 0;
    bool triggerObserved = false;
};

// 将有限线性采集样本解码为 VCD（T-P3-03）。
// The current RTL fills a finite linear buffer, addresses 0..depth-1, then stops.
// triggerIndex marks a location in that sequence; it is not a timestamp and
// does not imply a circular/pretrigger buffer. No trigger-based rotation occurs.
// 输出与 WavePanel 的 VcdLazyTraceSource 兼容（$timescale/$scope/$var/值变更）。
bool DecodeCaptureToVcd(const std::vector<std::uint32_t>& samples,
                        std::uint32_t depth, std::uint16_t triggerIndex,
                        const std::vector<CaptureProbe>& probes,
                        const std::string& path, CaptureVcdMeta& meta,
                        std::string& error, const CaptureTimebase& timebase = {});

bool SaveCaptureMetadata(const std::string& path, const CaptureVcdMeta& meta,
                         std::uint32_t depth, std::uint16_t triggerIndex,
                         std::string& error);

// capture.raw 读取（与 SaveCaptureRaw 配套，格式见 DebugProtocol.h）。
bool LoadCaptureRaw(const std::string& path, std::uint32_t& depth, std::uint32_t& width,
                    std::uint16_t& start, std::vector<std::uint32_t>& samples,
                    std::string& error);

} // namespace debug
} // namespace sigflow
