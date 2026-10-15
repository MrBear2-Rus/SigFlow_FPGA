#pragma once

#include "DebugContract.h"
#include "ProbePlanner.h"
#include "RootCauseGraph.h"

namespace sigflow::debug {

struct ProbePlanningRequest {
    DebugContract contract;
    std::vector<DebugProbe> requestedProbes;
    // TB-27 implementation feedback.  A measured delta is kept separate
    // from estimates so the planner can lower risky optional probes without
    // inventing per-signal LUT numbers.
    std::vector<ProbeImpactHint> impactHints;
    // TB-23/root-cause suggestions that can be evaluated together with the
    // current budget.  They remain optional until a user explicitly applies
    // the resulting draft.
    std::vector<RootCauseProbeSuggestion> evidenceSuggestions;
    std::string symptomSignal;
    std::vector<std::string> excludedSignals;
    std::size_t capacityBits = 32;
    std::size_t maxUpstreamDepth = 4;
};

class ProbePlanningService {
public:
    // Existing contract probes stay required and retain their packing order.
    // Unknown sidecar width remains unknown; no guessed RTL widths are used.
    static bool Plan(const ProbePlanningRequest& request, const SignalSourceMap* sourceMap,
                     const DependencyGraph* graph, ProbePlan& result, std::string& error);
    static bool BuildDraft(const DebugContract& base, const ProbePlan& plan,
                           DebugContract& draft, std::string& error);
};

} // namespace sigflow::debug
