#include "ProbePlanningService.h"

#include <algorithm>
#include <cmath>
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

bool AppendReportSuggestions(const ProbePlanningRequest& request,
                             const SignalSourceMap* sourceMap,
                             std::vector<RootCauseProbeSuggestion>& suggestions,
                             std::string& error)
{
    if (!request.evidenceReport) return true;
    const auto& report = *request.evidenceReport;
    if (report.schemaVersion != "1.0" || report.analyzerVersion != "1.0" ||
        report.rulesVersion != "1.0" || report.captureId.empty() || report.analysisId.empty() ||
        report.inputFingerprint.empty() || report.firstDifferenceSignal.empty() ||
        report.maxDepth == 0 || report.maxCandidates == 0 ||
        report.candidates.size() > report.maxCandidates || report.candidates.size() > 512) {
        error = "invalid or unsupported evidence report for probe planning";
        return false;
    }
    if (!request.expectedEvidenceCaptureId.empty() &&
        request.expectedEvidenceCaptureId != report.captureId) {
        error = "evidence report capture identity does not match the planning context";
        return false;
    }
    std::set<std::string> seen;
    for (const auto& candidate : report.candidates) {
        const auto& location = candidate.candidate.location;
        if (location.signalName.empty() || !std::isfinite(candidate.evidenceScore)) {
            error = "invalid evidence candidate for probe planning";
            return false;
        }
        const bool explicitPath = location.sftreePath.find('.') != std::string::npos;
        std::string path = explicitPath ? location.sftreePath : location.signalName;
        const SignalLocation* mapped = nullptr;
        if (sourceMap) {
            for (const auto& entry : sourceMap->Entries()) {
                if ((explicitPath && Canonical(request.contract, entry) == path) ||
                    (!explicitPath && entry.signalName == location.signalName)) {
                    if (mapped && Canonical(request.contract, *mapped) != Canonical(request.contract, entry)) {
                        error = "evidence candidate has an ambiguous instance path: " + location.signalName;
                        return false;
                    }
                    mapped = &entry;
                }
            }
        }
        if (mapped) path = Canonical(request.contract, *mapped);
        if (!seen.insert(path).second) {
            error = "duplicate evidence candidate: " + path;
            return false;
        }
        if (candidate.observation == EvidenceObservationState::Observed) continue;
        RootCauseProbeSuggestion suggestion;
        suggestion.signal = path;
        suggestion.kind = "evidence-supplement";
        suggestion.clockDomain = location.clockDomain;
        suggestion.width = location.width;
        suggestion.widthKnown = location.width != 0;
        suggestion.priority = 100 - static_cast<int>(std::min<std::size_t>(9, candidate.candidate.graphDistance) * 10);
        if (candidate.observation == EvidenceObservationState::Unknown) suggestion.priority -= 5;
        suggestion.distinguishingCandidates.push_back(location.signalName);
        suggestion.reason = "observe " + path + " to investigate candidate from capture " + report.captureId;
        for (const auto& missing : candidate.missingEvidence) suggestion.reason += "; missing: " + missing;
        if (path.find('.') == std::string::npos) suggestion.reason += "; explicit instance path is required";
        suggestions.push_back(std::move(suggestion));
    }
    return true;
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
    std::vector<RootCauseProbeSuggestion> suggestions = request.evidenceSuggestions;
    if (!AppendReportSuggestions(request, sourceMap, suggestions, error)) return false;
    std::map<std::string, const ProbeImpactHint*> impactBySignal;
    for (const auto& hint : request.impactHints) {
        if (hint.signal.empty() || !impactBySignal.emplace(hint.signal, &hint).second) {
            error = "impact hints require unique nonempty signal paths";
            return false;
        }
        if (hint.measuredLutDeltaAvailable && hint.evidencePath.empty()) {
            error = "measured probe cost requires an evidence path: " + hint.signal;
            return false;
        }
    }
    std::set<std::string> excluded(request.excludedSignals.begin(), request.excludedSignals.end());
    std::map<std::string, const DebugProbe*> existing;
    std::vector<ProbeCandidate> candidates;
    std::vector<ProbeRequirement> requirements;
    auto applyImpactHint = [&](ProbeCandidate& candidate) {
        const auto found = impactBySignal.find(candidate.signal);
        if (found != impactBySignal.end()) {
            const auto& hint = *found->second;
            candidate.fanout = std::max(candidate.fanout, hint.fanout);
            candidate.estimatedLutCost = hint.estimatedLutCost;
            candidate.measuredLutDelta = hint.measuredLutDelta;
            candidate.measuredLutDeltaAvailable = hint.measuredLutDeltaAvailable;
            candidate.combinationalObservation = hint.combinationalObservation;
            candidate.clockSensitive = hint.clockSensitive;
            candidate.resetSensitive = hint.resetSensitive;
            if (!hint.evidencePath.empty()) candidate.impactEvidencePath = hint.evidencePath;
        }
    };
    bool withheldUnqualified = false;
    for (const auto& probe : request.contract.probes) {
        existing[probe.path] = &probe;
        ProbeCandidate candidate{probe.path, Domain(request.contract, probe), probe.width,
                                true, probe.width != 0, excluded.count(probe.path) != 0, true};
        applyImpactHint(candidate);
        candidates.push_back(std::move(candidate));
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
            ProbeCandidate candidate{path, domain, width, true, width != 0,
                                     excluded.count(path) != 0, probe != nullptr};
            candidate.fanout = location.fanout;
            if (location.fanout != 0) candidate.impactEvidencePath = request.sourceMapPath;
            applyImpactHint(candidate);
            candidates.push_back(std::move(candidate));
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
    for (const auto& suggestion : suggestions) {
        if (suggestion.signal.empty()) continue;
        ProbeRequirement requirement;
        requirement.signal = suggestion.signal;
        if (sourceMap && suggestion.signal.find('.') == std::string::npos) {
            if (const auto* location = sourceMap->Find(suggestion.signal)) {
                requirement.signal = Canonical(request.contract, *location);
            }
        }
        requirement.reason = suggestion.reason.empty()
            ? "supplementary evidence for root-cause candidates" : suggestion.reason;
        requirement.kind = "evidence-supplement";
        requirement.clockDomain = suggestion.clockDomain;
        requirement.width = suggestion.widthKnown ? suggestion.width : 0;
        requirement.priority = suggestion.priority;
        requirement.required = false;
        requirement.distinguishingCandidates = suggestion.distinguishingCandidates;
        requirements.push_back(std::move(requirement));
    }
    if (!ProbePlanner::Plan(requirements, candidates, request.capacityBits, result, error)) return false;
    result.contractSnapshot = request.contract.ToJson();
    if (sourceMap) result.diagnostics.push_back("source map is static planning evidence; new probes require build validation");
    if (!sourceMap) result.diagnostics.push_back("no source map: only existing contract probes have authoritative width/visibility");
    if (withheldUnqualified) result.diagnostics.push_back("unqualified source records withheld: an explicit instance path is required");
    if (!request.symptomSignal.empty() && !graph) result.diagnostics.push_back("no dependency graph: upstream recommendations unavailable");
    if (!request.impactHints.empty()) {
        result.diagnostics.push_back("TB-27 impact hints are advisory; measured deltas are kept separate from estimated fanout/cost");
    }
    if (!suggestions.empty()) {
        result.evidenceSupplementUsed = true;
        result.diagnostics.push_back("TB-23 evidence supplement suggestions are optional and require explicit user application");
    }
    if (request.evidenceReport) {
        const auto& report = *request.evidenceReport;
        result.evidenceSource = {request.evidenceReportPath, report.captureId, report.analysisId,
                                 report.inputFingerprint, report.analyzerVersion, report.rulesVersion};
        result.diagnostics.push_back("imported evidence keeps its original capture/analysis identity; current build identity is unverified and new observations require build validation");
        if (suggestions.empty()) result.diagnostics.push_back("evidence report has no unobserved/unknown candidates; missing alignment or completeness alone does not require another probe");
    }
    if (request.impactReport) {
        const auto& report = *request.impactReport;
        if (report.schemaVersion != "1.0") { error = "unsupported impact report version"; return false; }
        auto& context = result.buildImpact;
        context.reportPath = request.impactReportPath;
        switch (report.comparability) {
            case InstrumentationComparability::Comparable: context.comparability = "comparable"; break;
            case InstrumentationComparability::ObservationOnly: context.comparability = "observation_only"; break;
            case InstrumentationComparability::NotComparable: context.comparability = "not_comparable"; break;
        }
        context.gatePass = report.gate.pass;
        context.gateOverridden = report.gate.overridden;
        context.gateReasons = report.gate.reasons;
        for (const auto& reason : report.blockingDifferences) context.gateReasons.push_back(reason);
        for (const auto& resource : report.resources) {
            context.resourceSummary.push_back(resource.normalizedName + ": " +
                (resource.available ? std::to_string(resource.baselineUsed) + " -> " +
                    std::to_string(resource.instrumentedUsed) + " (delta=" +
                    std::to_string(resource.absoluteDelta) + ")" : "unavailable"));
        }
        result.diagnostics.push_back("imported TB-27 build totals are context only; they are not per-probe measurements and do not override build gates");
    }
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
