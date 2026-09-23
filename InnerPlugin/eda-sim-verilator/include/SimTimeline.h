#pragma once

#include "StimulusModel.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace eda {
namespace sim {

struct TimelineEvent {
    std::uint64_t time = 0;
    std::string signalName;
    std::string value;
    std::string comment;
};

struct TimelineClock {
    std::string signalName;
    std::uint64_t halfPeriod = 0;
    std::string initialValue;
    bool valid = false;
};

struct Timeline {
    std::string topModuleName;
    std::vector<TimelineEvent> events;
    TimelineClock clock;
    std::uint64_t maxSimTime = 0;
    std::vector<std::string> inputSignals;
    std::map<std::string, std::string> signalInitValues;
};

// 自 `main/Simulation/TimelineGenerator` 移入并去 wx 化。
class TimelineGenerator {
public:
    TimelineGenerator();
    ~TimelineGenerator();

    Timeline Generate(const TestbenchInfo& tbInfo, const std::string& topModule);
    const std::string& LastError() const { return lastError_; }

private:
    std::string lastError_;
    void FlattenInitialBlock(const InitialBlock& block, const TestbenchInfo& tbInfo,
                             std::vector<TimelineEvent>& events, std::uint64_t& currentTime);
    std::string MapSignalToPort(const std::string& tbSignal, const TestbenchInfo& tbInfo);
};

} // namespace sim
} // namespace eda
