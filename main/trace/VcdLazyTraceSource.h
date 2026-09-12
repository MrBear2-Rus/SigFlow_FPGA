#pragma once

#include "TraceSource.h"
#include "TraceSidecarIndex.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sigflow {
namespace trace {

// 懒加载 VCD 源（W1-01/02/03）：
// - 头部解析仅读元数据；
// - 时间索引按字节跨度采样，查询时从最近的采样点 seek 后局部解析；
// - 侧车 .bwidx 缓存头部信息与时间索引，二次打开免全量扫描。
class VcdLazyTraceSource : public TraceSource {
public:
    VcdLazyTraceSource() = default;
    ~VcdLazyTraceSource() override = default;

    bool Open(const std::string& path, std::string& error) override;

    const std::vector<SignalInfo>& Signals() const override { return m_signals; }
    const SignalInfo* SignalById(int id) const override;
    TraceTimeRange TimeRange() const override { return m_timeRange; }
    std::string Timescale() const override { return m_timescale; }
    const std::string& Path() const override { return m_path; }
    bool HasSidecar() const override { return m_sidecarLoaded; }

    bool Query(const SignalInfo& signal, TimeValue t0, TimeValue t1,
               std::vector<Transition>& out, std::string& error) override;
    bool ValueAt(const SignalInfo& signal, TimeValue t,
                 std::string& value, std::string& error) override;

    std::uint64_t IndexScanBytes() const { return m_indexScanBytes; }
    std::uint64_t SampleStrideBytes() const { return m_sampleStrideBytes; }

private:
    bool ScanHeader(std::ifstream& in, std::string& error);
    bool BuildTimeIndex(std::ifstream& in);
    std::uint64_t StartOffsetForTime(TimeValue t) const;
    bool ParseValueChange(const std::string& line, const std::string& idCode,
                          std::string& value) const;

    static void TrimLine(std::string& line);
    static TimeValue ParseTime(const std::string& text);

    std::string m_path;
    std::vector<SignalInfo> m_signals;
    std::unordered_map<std::string, int> m_idToSignal;
    std::unordered_map<int, int> m_idToIndex;
    TraceTimeRange m_timeRange;
    std::string m_timescale = "1ns";
    std::uint64_t m_dataOffset = 0;
    std::uint64_t m_fileSize = 0;
    std::int64_t m_fileMtime = 0;
    std::vector<TraceSidecarIndex::TimeSample> m_timeIndex;
    std::uint64_t m_sampleStrideBytes = 64 * 1024;
    std::uint64_t m_indexScanBytes = 0;
    bool m_sidecarLoaded = false;
};

} // namespace trace
} // namespace sigflow
