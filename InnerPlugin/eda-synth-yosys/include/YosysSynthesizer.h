#pragma once

#include <eda/api/capabilities.h>
#include <eda/api/IPluginInteraction.h>
#include <eda/api/jobs.hpp>

namespace eda {
namespace synth {

// P1-1：eda-synth-yosys 官方插件。同时实现 ISynthesizer（类型化能力）与 IJobProvider（Job 执行体）。
class YosysSynthesizer final : public ISynthesizer, public IJobProvider {
public:
    // ISynthesizer
    std::string synthesizerId() const override { return "yosys"; }

    // IJobProvider
    std::string jobType() const override { return "synth"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest& request, JobContext& ctx) override;

    // IPluginInteraction
    void invoke(const MethodCall& call, Callback<Error, Json> onReply) override;
    std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&> handler) override;
    void unsubscribe(std::uint64_t subscriptionId) override;
    PluginInfo info() const override;
};

} // namespace synth
} // namespace eda
