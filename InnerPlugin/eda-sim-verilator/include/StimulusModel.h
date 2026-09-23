#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eda {
namespace sim {

enum class StimulusEventType {
    SignalAssign,
    Delay,
    PosedgeWait,
    NegedgeWait,
    Finish,
    Display,
    ClockDef,
};

struct StimulusEvent {
    StimulusEventType type = StimulusEventType::SignalAssign;
    std::string signalName;
    std::string value;
    std::uint64_t delayValue = 0;
    std::string edgeSignal;
    std::string rawText;
};

struct ClockDef {
    std::string signalName;
    std::uint64_t halfPeriod = 0;
    std::string initialValue;
};

struct InitialBlock {
    std::vector<StimulusEvent> events;
    bool hasClock = false;
    ClockDef clock;
};

struct TBPortMapping {
    std::string portName;
    std::string signalName;
};

struct TestbenchInfo {
    std::string moduleName;
    std::string topInstanceName;
    std::string topModuleName;
    std::vector<TBPortMapping> portMappings;
    std::vector<std::string> regSignals;
    std::vector<std::string> wireSignals;
    std::vector<InitialBlock> initialBlocks;
    std::uint64_t maxSimTime = 0;
};

// 自 `main/Simulation/StimulusParser` 移入并去 wx 化。
class StimulusParser {
public:
    StimulusParser();
    ~StimulusParser();

    bool Parse(const std::string& filePath, TestbenchInfo& result);
    bool ParseCode(const std::string& code, TestbenchInfo& result);
    const std::string& LastError() const { return lastError_; }

private:
    std::string lastError_;

    std::string Preprocess(const std::string& code);
    void ParseSignalDeclarations(const std::string& code, TestbenchInfo& info);
    void ParseModuleInstantiation(const std::string& code, TestbenchInfo& info);
    void ParseInitialBlocks(const std::string& code, TestbenchInfo& info);
    InitialBlock ParseSingleInitialBlock(const std::string& blockContent);
    StimulusEvent ParseAssignment(const std::string& line);
    std::uint64_t EstimateMaxSimTime(const TestbenchInfo& info);
};

} // namespace sim
} // namespace eda
