#pragma once

#include <eda/api/IPluginInteraction.h>
#include <eda/api/jobs.hpp>

namespace eda {
namespace sim {

// P1-4：运行仿真可执行文件并采集 VCD（jobType=sim.run）。
class VerilatorRunJob final : public IJobProvider, public IPluginInteraction {
public:
    std::string jobType() const override { return "sim.run"; }
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
