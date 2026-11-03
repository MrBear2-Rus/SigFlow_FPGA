#pragma once
#include <eda/api/jobs.hpp>

namespace eda::pnr {
class NextpnrProvider final : public IJobProvider {
  public:
    std::string jobType() const override { return "pnr"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest& request, JobContext& context) override;
};
}  // namespace eda::pnr
