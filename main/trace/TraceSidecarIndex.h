#pragma once

#include "TraceTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace trace {

// .bwidx 侧车索引（W1-03）：缓存 VCD 头部信息与时间索引，避免二次全量扫描。
class TraceSidecarIndex {
public:
    struct SignalEntry {
        std::string idCode;
        std::string name;
        std::string scope;
        unsigned width = 1;
        SignalKind kind = SignalKind::Scalar;
    };

    struct TimeSample {
        TimeValue time = 0;
        std::uint64_t offset = 0;
    };

    static constexpr std::uint32_t kMagic = 0x53465431; // "SFT1"
    static constexpr std::uint32_t kVersion = 1;

    std::string sourcePath;
    std::uint64_t sourceSize = 0;
    std::int64_t sourceMtime = 0;
    std::string timescale;
    TimeValue maxTime = 0;
    std::uint64_t dataOffset = 0;
    std::vector<SignalEntry> signals;
    std::vector<TimeSample> samples;

    bool Load(const std::string& path, std::string& error);
    bool Save(const std::string& path, std::string& error) const;

    static std::string SidecarPathFor(const std::string& vcdPath);
};

} // namespace trace
} // namespace sigflow
