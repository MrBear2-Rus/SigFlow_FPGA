#pragma once

#include <eda/api/capabilities.h>
#include <eda/api/IPluginInteraction.h>
#include <eda/api/jobs.hpp>

namespace eda {
namespace sim {

// P1-4：eda-sim-verilator 官方插件（第一增量：sim.build）。
// 去 vcvars 硬编码与死 DLL；经 IProcessHost 调用 verilator。
class VerilatorSimulator final : public ISimulator, public IJobProvider {
public:
    std::string simulatorId() const override { return "verilator"; }

    std::string jobType() const override { return "sim.build"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest& request, JobContext& ctx) override;

    void invoke(const MethodCall& call, Callback<Error, Json> onReply) override;
    std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&> handler) override;
    void unsubscribe(std::uint64_t subscriptionId) override;
    PluginInfo info() const override;
};

} // namespace sim
} // namespace eda
