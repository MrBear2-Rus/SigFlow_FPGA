#pragma once

#include <cstdint>

namespace sigflow {
namespace debug {

// P0-04 默认阈值表（可被契约覆盖并记录覆盖历史）。
namespace DebugDefaults {
inline constexpr unsigned kProbeTotalBits = 32;
inline constexpr std::uint32_t kCaptureDepth = 1024;
inline constexpr std::uint32_t kPretriggerSamples = 512;
inline constexpr std::uint32_t kUartBaud = 921600;
inline constexpr double kTimingSlackThresholdNs = 1.0;
inline constexpr double kAlignmentQualityThreshold = 0.9;
inline constexpr std::uint32_t kMaxSessionsKept = 20;
} // namespace DebugDefaults

} // namespace debug
} // namespace sigflow
