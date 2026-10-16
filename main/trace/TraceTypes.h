#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace trace {

using TimeValue = std::uint64_t;

enum class SignalKind {
    Scalar,
    Vector,
    Real,
    String,
};

struct SignalInfo {
    int id = -1;           // 源内稳定编号
    std::string name;      // 叶子名，如 clk
    std::string scope;     // 层级路径，如 TOP/TOP.full_adder
    std::string fullName;  // scope + name，如 TOP.clk
    std::string idCode;    // VCD 标识符
    unsigned width = 1;
    SignalKind kind = SignalKind::Scalar;
};

struct Transition {
    TimeValue time = 0;
    std::string value;     // 标量 "0"/"1"/"x"/"z"；向量为二进制串；real/string 原样
};

struct TraceTimeRange {
    TimeValue begin = 0;
    TimeValue end = 0;
    bool valid = false;
};

// Optional acquisition coverage metadata. A source may leave this unknown
// when the file format cannot distinguish an intentional idle period from a
// missing capture interval. When known, ranges are inclusive and describe
// portions of the requested time domain for which samples are trustworthy.
struct TraceCoverageRange {
    TimeValue begin = 0;
    TimeValue end = 0;
    std::string reason;
};

struct TraceCoverageInfo {
    bool known = false;
    std::vector<TraceCoverageRange> covered;

    // Returns the first uncovered interval in [begin, end], if any. Unknown
    // coverage deliberately returns no gap so legacy sources retain their
    // previous behavior.
    bool FindGap(TimeValue begin, TimeValue end, TraceCoverageRange& gap) const;
};

} // namespace trace
} // namespace sigflow
