#pragma once

#include <eda/api/waveform.hpp>

#include "TraceSource.h"

#include <memory>

namespace sigflow {
namespace trace {

// P1-9：把契约层 IWaveformBackend（如 eda-wave-vcd 的 VcdWaveformBackend）适配为现有 TraceSource，
// 使波形数据层可以平滑切换到非定容、RAII 的新后端。
class WaveformBackendTraceSource final : public TraceSource {
public:
    explicit WaveformBackendTraceSource(std::shared_ptr<eda::IWaveformBackend> backend);

    bool Open(const std::string& path, std::string& error) override;
    const std::vector<SignalInfo>& Signals() const override { return signals_; }
    const SignalInfo* SignalById(int id) const override;
    TraceTimeRange TimeRange() const override;
    std::string Timescale() const override { return timescale_; }
    const std::string& Path() const override { return path_; }

    bool Query(const SignalInfo& signal, TimeValue t0, TimeValue t1,
               std::vector<Transition>& out, std::string& error) override;
    bool ValueAt(const SignalInfo& signal, TimeValue t, std::string& value,
                 std::string& error) override;

    bool HasSidecar() const override { return false; }

private:
    std::shared_ptr<eda::IWaveformBackend> backend_;
    std::vector<SignalInfo> signals_;
    std::string timescale_;
    std::string path_;
    TraceTimeRange timeRange_;
};

} // namespace trace
} // namespace sigflow
