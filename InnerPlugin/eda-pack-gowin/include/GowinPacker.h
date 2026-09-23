#pragma once

#include <eda/api/capabilities.h>
#include <eda/api/IPluginInteraction.h>
#include <eda/api/jobs.hpp>

namespace eda {
namespace pack {

// P1-3：eda-pack-gowin 官方插件（Apicula gowin_pack）。
class GowinPacker final : public IPacker, public IJobProvider {
public:
    std::string packerId() const override { return "gowin_pack"; }

    std::string jobType() const override { return "pack"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest& request, JobContext& ctx) override;

    void invoke(const MethodCall& call, Callback<Error, Json> onReply) override;
    std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&> handler) override;
    void unsubscribe(std::uint64_t subscriptionId) override;
    PluginInfo info() const override;
};

} // namespace pack
} // namespace eda
