#pragma once

#include <eda/api/capabilities.h>
#include <eda/api/IPluginInteraction.h>
#include <eda/api/jobs.hpp>

namespace eda {
namespace program {

// P1-3：eda-program-openfpgaloader 官方插件。烧录实板，必须显式确认（requireConfirm）。
class OpenFpgaLoaderProgrammer final : public IProgrammer, public IJobProvider {
public:
    std::string programmerId() const override { return "openfpgaloader"; }
    bool requiresConfirm() const override { return true; }

    std::string jobType() const override { return "flash"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest& request, JobContext& ctx) override;

    void invoke(const MethodCall& call, Callback<Error, Json> onReply) override;
    std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&> handler) override;
    void unsubscribe(std::uint64_t subscriptionId) override;
    PluginInfo info() const override;
};

} // namespace program
} // namespace eda
