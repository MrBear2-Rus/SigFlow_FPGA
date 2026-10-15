#include "ProbePlanningService.h"

#include <algorithm>
#include <map>
#include <set>

namespace sigflow::debug {
namespace {
std::string Canonical(const DebugContract& contract, const SignalLocation& location)
{
    if (location.sftreePath.find('.') != std::string::npos) return location.sftreePath;
    if (location.signalName.find('.') != std::string::npos) return location.signalName;
    return contract.topModule + "." + location.signalName;
}

std::string Domain(const DebugContract& contract, const DebugProbe& probe)
{
    if (!probe.clockDomain.empty()) return probe.clockDomain;
    if (contract.clockDomains.size() == 1) return contract.clockDomains.front().id;
    return contract.clockDomains.empty() ? contract.sampleClock.signal : "";
}
} // namespace

bool ProbePlanningService::Plan(const ProbePlanningRequest& request, const SignalSourceMap* sourceMap,
                                const DependencyGraph* graph, ProbePlan& result, std::string& error)
{
    result = ProbePlan();
    error.clear();
    if (!request.contract.Validate(error)) return false;
    if (request.capacityBits > 32) { error = "current debug core supports at most 32 probe bits"; return false; }
    if (request.maxUpstreamDepth > 32) { error = "upstream depth exceeds 32"; return false; }
    if (sourceMap && !sourceMap->TopModule().empty() && sourceMap->TopModule() != request.contract.topModule) {
        error = "signal map top module does not match the contract";
        return false;
    }
    if (graph && !graph->TopModule().empty() && graph->TopModule() != request.contract.topModule) {
        error = "dependency graph top module does not match the contract"; return false;
    }
    std::set<std::string> excluded(request.excludedSignals.begin(), request.excludedSignals.end());
    std::map<std::string, const DebugProbe*> existing;
    std::vector<ProbeCandidate> candidates;
    std::vector<ProbeRequirement> requirements;
    bool withheldUnqualified = false;
    for (const auto& probe : request.contract.probes) {
        existing[probe.path] = &probe;
        candidates.push_back({probe.path, Domain(request.contract, probe), probe.width,
                              true, probe.width != 0, excluded.count(probe.path) != 0, true});
        requirements.push_back({probe.path, "preserve existing probe and bit offsets", "context",
                                Domain(request.contract, probe), probe.width, 100, true});
    }
    if (sourceMap) {
        for (const auto& location : sourceMap->Entries()) {
            const auto path = Canonical(request.contract, location);
            const auto current = existing.find(path);
            if (location.sftreePath.find('.') == std::string::npos &&
                location.signalName.find('.') == std::string::npos && current == existing.end()) {
                withheldUnqualified = true;
                continue;
            }
            const auto* probe = current == existing.end() ? nullptr : current->second;
            const unsigned width = location.width != 0 ? location.width : probe ? probe->width : 0;
            const std::string domain = !location.clockDomain.empty() ? location.clockDomain
                : probe ? Domain(request.contract, *probe) : "";
            candidates.push_back({path, domain, width, true, width != 0,
                                  excluded.count(path) != 0, probe != nullptr});
        }
    }
    for (const auto& probe : request.requestedProbes) {
        requirements.push_back({probe.path, "explicitly requested probe", "context",
            // Empty means defer to authoritative candidate domain, not the primary clock.
            probe.clockDomain, probe.width, 110, true});
    }
    for (const auto& path : {request.contract.trigger.hsValidPath, request.contract.trigger.hsReadyPath}) {
        if (!path.empty()) requirements.push_back({path, "handshake trigger dependency", "trigger", "", 1, 120, true});
    }
    if (!request.symptomSignal.empty()) {
        requirements.push_back({request.symptomSignal, "selected symptom evidence", "symptom", "", 0, 115, true});
        std::string graphKey = request.symptomSignal;
        if (sourceMap) {
            for (const auto& location : sourceMap->Entries()) {
                if (Canonical(request.contract, location) == request.symptomSignal) { graphKey = location.signalName; break; }
            }
        }
        if (graph) {
            for (const auto& [signal, distance] : graph->UpstreamWithDistance(graphKey, request.maxUpstreamDepth)) {
                const auto* location = sourceMap ? sourceMap->Find(signal) : nullptr;
                const std::string path = location ? Canonical(request.contract, *location)
                    : signal.find('.') != std::string::npos ? signal : request.contract.topModule + "." + signal;
                requirements.push_back({path, "upstream of " + request.symptomSignal + " at distance " + std::to_string(distance),
                                        "upstream", "", 0, 60 - static_cast<int>(distance), false});
            }
        }
    }
    if (!ProbePlanner::Plan(requirements, candidates, request.capacityBits, result, error)) return false;
    result.contractSnapshot = request.contract.ToJson();
    if (sourceMap) result.diagnostics.push_back("source map is static planning evidence; new probes require build validation");
    if (!sourceMap) result.diagnostics.push_back("no source map: only existing contract probes have authoritative width/visibility");
    if (withheldUnqualified) result.diagnostics.push_back("unqualified source records withheld: an explicit instance path is required");
    if (!request.symptomSignal.empty() && !graph) result.diagnostics.push_back("no dependency graph: upstream recommendations unavailable");
    if (result.feasible) {
        DebugContract draft;
        if (!BuildDraft(request.contract, result, draft, error)) return false;
        // BuildDraft keeps existing order; selection of a new path requires a build.
        result.requiresNewBuild = draft.probes.size() != request.contract.probes.size();
    }
    return true;
}

bool ProbePlanningService::BuildDraft(const DebugContract& base, const ProbePlan& plan,
                                     DebugContract& draft, std::string& error)
{
    error.clear();
    if (!plan.feasible) { error = "cannot apply an infeasible probe plan"; return false; }
    if (!base.Validate(error)) return false;
    if (!plan.contractSnapshot.empty() && plan.contractSnapshot != base.ToJson()) {
        error = "probe plan contract snapshot is stale"; return false;
    }
    draft = base;
    std::map<std::string, const ProbePlanEntry*> selected;
    for (const auto& entry : plan.entries) if (entry.selected) {
        if (entry.signal.empty() || entry.width == 0 || entry.clockDomain != Domain(base, base.probes.front()) ||
            !selected.emplace(entry.signal, &entry).second) {
            error = "invalid or duplicate selected probe"; return false;
        }
    }
    std::set<std::string> ids;
    for (const auto& probe : base.probes) {
        const auto it = selected.find(probe.path);
        if (it == selected.end() || it->second->width != probe.width ||
            it->second->clockDomain != Domain(base, probe)) {
            error = "plan must preserve existing probe packing and domains"; return false;
        }
        selected.erase(it);
        ids.insert(probe.id);
    }
    for (const auto& entry : plan.entries) {
        if (!entry.selected || selected.count(entry.signal) == 0) continue;
        DebugProbe probe;
        unsigned serial = static_cast<unsigned>(draft.probes.size());
        do { probe.id = "planned_" + std::to_string(++serial); } while (ids.count(probe.id));
        ids.insert(probe.id);
        probe.path = entry.signal;
        probe.width = entry.width;
        probe.clockDomain = entry.clockDomain;
        // The contract represents an implicit primary clock with an empty
        // domain. Keep that representation consistent across appended probes.
        if (std::all_of(base.probes.begin(), base.probes.end(), [](const auto& current) {
                return current.clockDomain.empty(); })) probe.clockDomain.clear();
        draft.probes.push_back(probe);
    }
    if (!draft.AssignProbeBitOffsets(error) || !draft.Validate(error)) return false;
    for (std::size_t i = 0; i < base.probes.size(); ++i) {
        if (draft.probes[i].bitOffset != base.probes[i].bitOffset) {
            error = "existing probe offsets are not densely packed; explicit remapping is required";
            return false;
        }
    }
    if (draft.probes.size() != base.probes.size()) {
        draft.fingerprints = DebugFingerprints();
        draft.sessionId = NewDebugSessionId();
    }
    return true;
}
} // namespace sigflow::debug
