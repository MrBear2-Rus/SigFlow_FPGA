#pragma once
#include <eda/api/jobs.hpp>
namespace eda::synth {
class YosysProvider final : public IJobProvider {
public:
    std::string jobType() const override { return "synth"; }
    Json paramsSchema() const override;
    Json resultSchema() const override;
    Error startJob(const JobRequest&, JobContext&) override;
};
}
