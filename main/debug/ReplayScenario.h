#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "vcd.h"
}

namespace sigflow {
namespace debug {

struct ReplaySignal {
    std::string sourceName;
    std::string portName;
    unsigned width = 1;
};

struct ReplayAssignment {
    std::uint64_t time = 0;
    std::string signalName;
    std::string value;
};

struct ReplayScenario {
    std::string clockSignal;
    std::uint64_t clockHalfPeriod = 0;
    std::string initialClockValue;
    std::vector<ReplaySignal> inputSignals;
    std::vector<ReplayAssignment> assignments;
    std::uint64_t maxTime = 0;
};

struct ReplayConsistencyReport {
    double score = 0.0;
    bool verified = false;
    std::size_t requestedInputs = 0;
    std::size_t capturedInputs = 0;
    std::size_t assignmentCount = 0;
    std::string message;
};

ReplayConsistencyReport EvaluateReplayConsistency(const ReplayScenario& scenario,
                                                  std::size_t requestedInputCount);

class ReplayStimulusExtractor {
public:
    static bool Extract(vcd_t* capture,
                        const std::vector<std::string>& inputSignals,
                        const std::string& clockSignal,
                        ReplayScenario& scenario,
                        std::string& error);
};

class ReplayTestbenchGenerator {
public:
    static bool Generate(const ReplayScenario& scenario,
                         const std::string& topModule,
                         const std::string& outputPath,
                         const std::string& vcdPath,
                         std::string& error);
};

struct ReplayRunResult {
    bool success = false;
    int exitCode = -1;
    std::string message;
};

class ReplayRunner {
public:
    static bool Run(const std::string& command,
                    const std::string& workingDirectory,
                    ReplayRunResult& result,
                    std::string& error);
};

} // namespace debug
} // namespace sigflow
