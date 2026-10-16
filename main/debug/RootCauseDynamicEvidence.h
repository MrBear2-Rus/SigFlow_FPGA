#pragma once

#include "RootCauseGraph.h"
#include "../trace/TraceSource.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow::debug {

// Bounded, deterministic extraction of observations around one already known
// symptom time.  This layer reads a trace only; it does not claim causality.
struct RootCauseDynamicEvidenceOptions {
    std::string captureId;
    std::string analysisId;
    sigflow::trace::TimeValue symptomTime = 0;
    sigflow::trace::TimeValue preWindow = 100;
    sigflow::trace::TimeValue postWindow = 0;
    sigflow::trace::TimeValue pipelineLatency = 0;
    bool latencyKnown = false;
    bool requirePhysicalTime = true;
    int resetSignalId = -1;
    bool resetActiveHigh = true;
    int validSignalId = -1;
    int readySignalId = -1;
};

bool ExtractRootCauseDynamicEvidence(
    sigflow::trace::TraceSource& source,
    const std::vector<std::string>& candidateSignals,
    const RootCauseDynamicEvidenceOptions& options,
    std::vector<RootCauseEvidenceObservation>& out,
    std::string& error);

} // namespace sigflow::debug
