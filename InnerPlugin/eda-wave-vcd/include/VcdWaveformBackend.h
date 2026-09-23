#pragma once

#include <eda/api/waveform.hpp>

#include <map>
#include <string>
#include <vector>

namespace eda {
namespace wave {

// P1-9：wx-free VCD 后端。无定容截断（数据动态增长），资源由 std 容器管理（RAII）。
class VcdWaveformBackend final : public IWaveformBackend {
public:
    bool Open(const std::string& path, std::string& error) override;
    const std::vector<WaveSignal>& Signals() const override { return signals_; }
    std::string Timescale() const override { return timescale_; }
    WaveTimeRange TimeRange() const override { return timeRange_; }

    bool Query(int signalId, std::uint64_t t0, std::uint64_t t1,
               std::vector<WaveTransition>& out, std::string& error) override;
    bool ValueAt(int signalId, std::uint64_t t, std::string& value,
                 std::string& error) override;

private:
    std::vector<WaveSignal> signals_;
    std::vector<std::vector<WaveTransition>> changes_;  // 与 signals_ 同序
    std::map<std::string, int> idToIndex_;
    std::string timescale_;
    WaveTimeRange timeRange_;
};

} // namespace wave
} // namespace eda
