#pragma once

#include "SimTimeline.h"

#include <string>

namespace eda {
namespace sim {

// 自 `main/Simulation/SimMainGenerator` 移入并去 wx 化。
class SimMainGenerator {
public:
    SimMainGenerator();
    ~SimMainGenerator();

    bool Generate(const Timeline& timeline, const std::string& outputPath,
                  const std::string& vcdRelativePath = "waveform/wave.vcd");
    const std::string& LastError() const { return lastError_; }

private:
    std::string lastError_;

    std::string GenerateIncludes(const Timeline& timeline);
    std::string GenerateInitialization(const Timeline& timeline);
    std::string GenerateEventDriving(const Timeline& timeline);
    std::string GenerateCleanup();
    std::string VerilogValueToCpp(const std::string& value);
};

} // namespace sim
} // namespace eda
