#pragma once

#include <eda/api/capabilities.h>
#include <eda/api/IPluginInteraction.h>
#include <eda/api/jobs.hpp>

namespace eda {
namespace pnr {

// P1-2：eda-pnr-nextpnr 官方插件。同时实现 IPlaceAndRouter 与 IJobProvider（jobType=pnr）。
class NextpnrPlaceRouter final : public IPlaceAndRouter, public IJobProvider {
public:
    // IPlaceAndRouter
    std::string pnrId() const override { return "nextpnr-himbaechel"; }

    // IJobProvider
    std::string jobType() const override { return "pnr"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest& request, JobContext& ctx) override;

    // IPluginInteraction
    void invoke(const MethodCall& call, Callback<Error, Json> onReply) override;
    std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&> handler) override;
    void unsubscribe(std::uint64_t subscriptionId) override;
    PluginInfo info() const override;
};

} // namespace pnr
} // namespace eda
