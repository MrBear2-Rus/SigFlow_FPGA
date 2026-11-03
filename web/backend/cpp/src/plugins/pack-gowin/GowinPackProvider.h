#pragma once
#include <eda/api/jobs.hpp>

namespace eda::pack {
class GowinPackProvider final : public IJobProvider {
  public:
    std::string jobType() const override { return "pack"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest& request, JobContext& context) override;
};
}  // namespace eda::pack
