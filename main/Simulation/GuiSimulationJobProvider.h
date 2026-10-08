#pragma once

#include <eda/api/jobs.hpp>

#include <memory>

class SimulationEngine;

// 保留桌面版 DLL/激励仿真执行体，把作业生命周期交给 CoreJobService。
class GuiSimulationJobProvider final : public eda::IJobProvider {
public:
    GuiSimulationJobProvider(std::shared_ptr<SimulationEngine> engine, bool build);
    std::string jobType() const override;
    eda::Json paramsSchema() const override;
    eda::Json resultSchema() const override;
    eda::Error startJob(const eda::JobRequest& request, eda::JobContext& context) override;

private:
    std::shared_ptr<SimulationEngine> engine_;
    bool build_;
};
