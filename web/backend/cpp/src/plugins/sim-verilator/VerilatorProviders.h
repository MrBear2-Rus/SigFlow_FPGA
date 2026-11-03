#pragma once
#include <eda/api/jobs.hpp>
namespace eda::sim {
class BuildProvider final : public IJobProvider {
  public:
    std::string jobType() const override { return "sim.build"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest&, JobContext&) override;
};
class RunProvider final : public IJobProvider {
  public:
    std::string jobType() const override { return "sim.run"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest&, JobContext&) override;
};
}  // namespace eda::sim
