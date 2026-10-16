#include "RootCauseGraph.h"

#include <algorithm>
#include <fstream>
#include <json/json.h>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sigflow {
namespace debug {

void SignalSourceMap::Add(SignalLocation location) {
    for (auto& existing : locations_) {
        if (existing.signalName == location.signalName) {
            existing = std::move(location);
            return;
        }
    }
    locations_.push_back(std::move(location));
}

const SignalLocation* SignalSourceMap::Find(const std::string& signalName) const {
    for (const auto& location : locations_) {
        if (location.signalName == signalName) return &location;
    }
    return nullptr;
}

std::vector<std::string> SignalSourceMap::InputSignals() const {
    std::vector<std::string> result;
    for (const auto& location : locations_) {
        if (location.isInput && location.isProbe) result.push_back(location.signalName);
    }
    return result;
}

bool SignalSourceMap::LoadJson(const std::string& path, std::string& error) {
    error.clear();
    std::ifstream input(path);
    if (!input) { error = "cannot open signal source map: " + path; return false; }
    Json::Value root;
    Json::CharReaderBuilder builder;
    if (!Json::parseFromStream(builder, input, &root, &error) ||
        !root.isObject() || !root["signals"].isArray()) {
        error = "invalid signal source map: " + error; return false;
    }
    if ((root.isMember("schema_version") && (!root["schema_version"].isString() ||
        root["schema_version"].asString() != "1.0")) ||
        (root.isMember("top_module") && !root["top_module"].isString())) {
        error = "unsupported signal source map version or top_module"; return false;
    }
    std::vector<SignalLocation> parsed;
    std::set<std::string> names;
    for (const auto& item : root["signals"]) {
        if (!item.isObject() || !item["signal"].isString() || item["signal"].asString().empty()) {
            error = "source map signal requires a nonempty stable key"; return false;
        }
        for (const auto* key : {"source_path", "sftree_path", "canvas_id", "clock_domain", "direction"}) {
            if (item.isMember(key) && !item[key].isString()) {
                error = std::string("source map field must be a string: ") + key; return false;
            }
        }
        if ((item.isMember("source_line") && (!item["source_line"].isInt() || item["source_line"].asInt() < 0)) ||
            (item.isMember("fanout") && !item["fanout"].isUInt64()) ||
            (item.isMember("width") && !item["width"].isUInt()) ||
            (item.isMember("probe") && !item["probe"].isBool()) ||
            (item.isMember("is_input") && !item["is_input"].isBool())) {
            error = "invalid source map numeric or boolean metadata"; return false;
        }
        SignalLocation location;
        location.signalName = item["signal"].asString();
        if (!names.insert(location.signalName).second) {
            error = "duplicate source map signal: " + location.signalName; return false;
        }
        location.sourcePath = item.get("source_path", "").asString();
        location.sourceLine = item.get("source_line", 0).asInt();
        location.sftreePath = item.get("sftree_path", location.signalName).asString();
        location.canvasId = item.get("canvas_id", location.signalName).asString();
        location.clockDomain = item.get("clock_domain", "").asString();
        location.fanout = item.get("fanout", 0).asUInt64();
        location.isInput = item.get("direction", "").asString() == "input" ||
                           item.get("is_input", false).asBool();
        location.isProbe = item.get("probe", false).asBool();
        location.width = item.get("width", 0).asUInt();
        parsed.push_back(std::move(location));
    }
    locations_ = std::move(parsed);
    topModule_ = root.get("top_module", "").asString();
    return true;
}

std::size_t SignalSourceMap::Size() const { return locations_.size(); }

void DependencyGraph::AddNode(SignalLocation location) {
    for (auto& existing : nodes_) {
        if (existing.signalName == location.signalName) {
            existing = std::move(location);
            return;
        }
    }
    nodes_.push_back(std::move(location));
}

void DependencyGraph::AddDependency(const std::string& source, const std::string& dependent) {
    if (source.empty() || dependent.empty() || source == dependent) return;
    for (const auto& edge : edges_) {
        if (edge.source == source && edge.dependent == dependent) return;
    }
    edges_.push_back({source, dependent});
}

std::vector<std::pair<std::string, std::size_t>> DependencyGraph::UpstreamWithDistance(
    const std::string& signalName, std::size_t maxDepth) const {
    std::vector<std::pair<std::string, std::size_t>> result;
    std::queue<std::pair<std::string, std::size_t>> pending;
    std::set<std::string> visited;
    pending.push({signalName, 0});
    visited.insert(signalName);
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.pop();
        if (current.second >= maxDepth) continue;
        for (const auto& edge : edges_) {
            if (edge.dependent != current.first || visited.count(edge.source) != 0) continue;
            visited.insert(edge.source);
            result.push_back({edge.source, current.second + 1});
            pending.push({edge.source, current.second + 1});
        }
    }
    return result;
}

bool DependencyGraph::CollectUpstreamWithBounds(
    const std::string& signalName,
    std::size_t maxDepth,
    std::size_t maxNodes,
    const std::function<bool()>& shouldCancel,
    RootCauseTraversalResult& result,
    std::string& error) const
{
    error.clear();
    result = RootCauseTraversalResult();
    if (signalName.empty()) {
        error = "root-cause traversal requires a signal name";
        return false;
    }
    if (maxNodes == 0) {
        error = "root-cause traversal requires a positive node limit";
        return false;
    }

    std::unordered_set<std::string> visited;
    std::function<void(const std::string&, std::size_t, std::unordered_set<std::string>&)> visit;
    visit = [&](const std::string& current, std::size_t depth,
                std::unordered_set<std::string>& path) {
        if (result.cancelled || result.truncated) return;
        if (shouldCancel && shouldCancel()) {
            result.cancelled = true;
            return;
        }
        bool hasUpstream = false;
        for (const auto& edge : edges_) {
            if (edge.dependent == current) {
                hasUpstream = true;
                break;
            }
        }
        if (hasUpstream && depth >= maxDepth) {
            result.depthLimited = true;
            return;
        }
        for (const auto& edge : edges_) {
            if (edge.dependent != current) continue;
            if (path.count(edge.source) != 0) {
                result.cycleDetected = true;
                continue;
            }
            if (visited.count(edge.source) != 0) continue;
            if (result.upstream.size() >= maxNodes) {
                result.truncated = true;
                return;
            }
            visited.insert(edge.source);
            path.insert(edge.source);
            result.upstream.push_back({edge.source, depth + 1});
            ++result.visited;
            visit(edge.source, depth + 1, path);
            path.erase(edge.source);
            if (result.cancelled || result.truncated) return;
        }
    };

    std::unordered_set<std::string> path;
    path.insert(signalName);
    visit(signalName, 0, path);
    return true;
}

std::vector<std::string> DependencyGraph::Upstream(const std::string& signalName,
                                                   std::size_t maxDepth) const {
    std::vector<std::string> result;
    for (const auto& item : UpstreamWithDistance(signalName, maxDepth)) {
        result.push_back(item.first);
    }
    return result;
}

std::size_t DependencyGraph::NodeCount() const { return nodes_.size(); }

const char* ToString(EvidenceObservationState state)
{
    switch (state) {
        case EvidenceObservationState::Observed: return "observed";
        case EvidenceObservationState::Unobserved: return "unobserved";
        case EvidenceObservationState::Unknown: return "unknown";
    }
    return "unknown";
}

bool DependencyGraph::LoadJson(const std::string& path, std::string& error) {
    error.clear();
    std::ifstream input(path);
    if (!input) {
        error = "cannot open dependency graph: " + path;
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseError;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!reader->parse(content.data(), content.data() + content.size(), &root, &parseError) ||
        !root.isObject()) {
        error = "invalid dependency graph: " + parseError;
        return false;
    }
    if ((root.isMember("schema_version") && (!root["schema_version"].isString() ||
            root["schema_version"].asString() != "1.0")) ||
        (root.isMember("top_module") && !root["top_module"].isString())) {
        error = "unsupported dependency graph version or top_module";
        return false;
    }
    topModule_ = root.get("top_module", "").asString();
    nodes_.clear();
    edges_.clear();
    if (root["nodes"].isArray()) {
        for (const auto& item : root["nodes"]) {
            if (!item.isObject() || !item["signal"].isString()) continue;
            SignalLocation location;
            location.signalName = item["signal"].asString();
            location.clockDomain = item.get("clock_domain", "").asString();
            location.fanout = item.get("fanout", 0).asUInt64();
            AddNode(std::move(location));
        }
    }
    if (root["dependencies"].isArray()) {
        for (const auto& item : root["dependencies"]) {
            if (!item.isObject()) continue;
            AddDependency(item.get("source", "").asString(),
                          item.get("dependent", "").asString());
        }
    }
    return true;
}

std::vector<RootCauseCandidate> RootCauseAnalyzer::Rank(
    const std::string& firstDifferenceSignal,
    const DependencyGraph& graph,
    const SignalSourceMap& sourceMap,
    std::size_t maxDepth,
    std::size_t maxCandidates) {
    std::vector<RootCauseCandidate> result;
    const auto upstream = graph.UpstreamWithDistance(firstDifferenceSignal, maxDepth);
    for (const auto& upstreamItem : upstream) {
        if (maxCandidates != 0 && result.size() >= maxCandidates) break;
        RootCauseCandidate candidate;
        const SignalLocation* location = sourceMap.Find(upstreamItem.first);
        candidate.location = location ? *location : SignalLocation{upstreamItem.first, "", 0, upstreamItem.first, upstreamItem.first, "", 0};
        candidate.graphDistance = upstreamItem.second;
        const double distanceScore = 1.0 / static_cast<double>(candidate.graphDistance);
        const double fanoutPenalty = std::min(0.25, static_cast<double>(candidate.location.fanout) / 100.0);
        const double clockBonus = candidate.location.clockDomain.empty() ? 0.0 : 0.1;
        candidate.score = std::max(0.0, 0.9 * distanceScore - fanoutPenalty + clockBonus);
        candidate.reason = "upstream distance=" + std::to_string(candidate.graphDistance);
        result.push_back(std::move(candidate));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.graphDistance < right.graphDistance;
    });
    return result;
}

bool RootCauseAnalyzer::RankWithEvidence(
    const std::string& firstDifferenceSignal,
    const DependencyGraph& graph,
    const SignalSourceMap& sourceMap,
    const RootCauseEvidenceContext& context,
    std::vector<RootCauseEvidenceCandidate>& out,
    std::string& error,
    std::size_t maxDepth,
    std::size_t maxCandidates)
{
    error.clear();
    out.clear();
    if (context.captureId.empty() || context.analysisId.empty()) {
        error = "root-cause evidence requires captureId and analysisId";
        return false;
    }
    std::unordered_map<std::string, const RootCauseEvidenceObservation*> observations;
    for (const auto& observation : context.observations) {
        if (observation.signalName.empty()) {
            error = "root-cause evidence observation has an empty signalName";
            return false;
        }
        if (observation.captureId != context.captureId ||
            observation.analysisId != context.analysisId) {
            error = "root-cause evidence identity does not match context";
            return false;
        }
        if (!observations.emplace(observation.signalName, &observation).second) {
            error = "duplicate root-cause evidence observation: " + observation.signalName;
            return false;
        }
    }

    // Rank the complete bounded dependency frontier first, then apply the
    // candidate limit after evidence has had a chance to promote a deeper
    // but strongly supported cause.
    const auto baseCandidates = Rank(firstDifferenceSignal, graph, sourceMap,
                                     maxDepth, 0);
    out.reserve(baseCandidates.size());
    for (const auto& base : baseCandidates) {
        RootCauseEvidenceCandidate ranked;
        ranked.candidate = base;
        ranked.evidenceScore = base.score;
        const auto found = observations.find(base.location.signalName);
        if (found == observations.end()) {
            ranked.observation = EvidenceObservationState::Unknown;
            ranked.evidenceScore = std::max(0.0, ranked.evidenceScore - 0.05);
            ranked.missingEvidence.push_back("no observation recorded for this signal");
        } else {
            const auto& observation = *found->second;
            ranked.observation = observation.state;
            ranked.supportingEvidence = observation.supportingFacts;
            ranked.missingEvidence = observation.missingConditions;
            if (observation.mappingAvailable) ranked.supportingEvidence.push_back("source mapping available");
            else ranked.missingEvidence.push_back("source mapping unavailable");
            if (observation.timeAligned) ranked.supportingEvidence.push_back("time aligned");
            else ranked.missingEvidence.push_back("time alignment unavailable");
            if (observation.dataComplete) ranked.supportingEvidence.push_back("complete capture evidence");
            else ranked.missingEvidence.push_back("capture completeness unknown");
            if (observation.waveformChanged) ranked.supportingEvidence.push_back("waveform changed near symptom");
            if (observation.protocolError) ranked.supportingEvidence.push_back("protocol error observed");
            switch (observation.state) {
                case EvidenceObservationState::Observed:
                    ranked.evidenceScore += 0.25;
                    if (observation.waveformChanged) ranked.evidenceScore += 0.25;
                    if (observation.protocolError) ranked.evidenceScore += 0.20;
                    if (observation.timeAligned) ranked.evidenceScore += 0.15;
                    if (observation.dataComplete) ranked.evidenceScore += 0.10;
                    if (observation.mappingAvailable) ranked.evidenceScore += 0.10;
                    break;
                case EvidenceObservationState::Unobserved:
                    ranked.evidenceScore = std::max(0.0, ranked.evidenceScore - 0.15);
                    ranked.missingEvidence.push_back("signal was not observed in this capture");
                    break;
                case EvidenceObservationState::Unknown:
                    ranked.evidenceScore = std::max(0.0, ranked.evidenceScore - 0.05);
                    ranked.missingEvidence.push_back("observation state is unknown");
                    break;
            }
        }
        ranked.candidate.reason += std::string("; evidence=") + ToString(ranked.observation);
        out.push_back(std::move(ranked));
    }
    std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
        if (left.evidenceScore != right.evidenceScore) return left.evidenceScore > right.evidenceScore;
        if (left.candidate.graphDistance != right.candidate.graphDistance)
            return left.candidate.graphDistance < right.candidate.graphDistance;
        return left.candidate.location.signalName < right.candidate.location.signalName;
    });
    if (maxCandidates != 0 && out.size() > maxCandidates) out.resize(maxCandidates);
    return true;
}

bool RootCauseAnalyzer::SuggestProbes(
    const std::string& firstDifferenceSignal,
    const DependencyGraph& graph,
    const SignalSourceMap& sourceMap,
    const RootCauseEvidenceContext& context,
    std::vector<RootCauseProbeSuggestion>& out,
    std::string& error,
    std::size_t maxDepth,
    std::size_t maxSuggestions)
{
    error.clear();
    out.clear();
    if (maxSuggestions == 0) {
        error = "root-cause probe suggestions require a positive limit";
        return false;
    }
    std::vector<RootCauseEvidenceCandidate> candidates;
    if (!RankWithEvidence(firstDifferenceSignal, graph, sourceMap, context,
                          candidates, error, maxDepth, 0)) {
        return false;
    }
    std::unordered_set<std::string> emitted;
    for (const auto& candidate : candidates) {
        if (candidate.observation == EvidenceObservationState::Observed) continue;
        const auto& location = candidate.candidate.location;
        if (location.signalName.empty() || !emitted.insert(location.signalName).second) continue;
        RootCauseProbeSuggestion suggestion;
        suggestion.signal = location.signalName;
        suggestion.clockDomain = location.clockDomain;
        suggestion.width = location.width;
        suggestion.widthKnown = location.width != 0;
        suggestion.priority = static_cast<int>(100 - std::min<std::size_t>(99, candidate.candidate.graphDistance * 10));
        if (candidate.observation == EvidenceObservationState::Unknown) suggestion.priority -= 5;
        suggestion.distinguishingCandidates.push_back(location.signalName);
        std::string reason = "observe " + location.signalName + " to distinguish root-cause candidates";
        if (!candidate.missingEvidence.empty()) {
            reason += "; missing: ";
            for (std::size_t i = 0; i < candidate.missingEvidence.size(); ++i) {
                if (i != 0) reason += ", ";
                reason += candidate.missingEvidence[i];
            }
        }
        if (!suggestion.widthKnown) reason += "; width metadata is required before budgeting";
        suggestion.reason = std::move(reason);
        out.push_back(std::move(suggestion));
        if (out.size() >= maxSuggestions) break;
    }
    std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return left.signal < right.signal;
    });
    return true;
}

namespace {

RootCauseEvidenceObservation BenchmarkObservation(
    const std::string& signal, const std::string& captureId,
    const std::string& analysisId, EvidenceObservationState state,
    bool waveformChanged, bool protocolError, bool mappingAvailable,
    bool timeAligned, bool dataComplete, std::string fact,
    std::string missing)
{
    RootCauseEvidenceObservation observation;
    observation.signalName = signal;
    observation.captureId = captureId;
    observation.analysisId = analysisId;
    observation.state = state;
    observation.waveformChanged = waveformChanged;
    observation.protocolError = protocolError;
    observation.mappingAvailable = mappingAvailable;
    observation.timeAligned = timeAligned;
    observation.dataComplete = dataComplete;
    if (!fact.empty()) observation.supportingFacts.push_back(std::move(fact));
    if (!missing.empty()) observation.missingConditions.push_back(std::move(missing));
    return observation;
}

RootCauseBenchmarkCase MakeBenchmarkCase(
    std::string id, std::string label, std::string firstDifference,
    std::string expected, std::string risk,
    RootCauseEvidenceObservation observation)
{
    RootCauseBenchmarkCase result;
    result.id = std::move(id);
    result.label = std::move(label);
    result.firstDifferenceSignal = std::move(firstDifference);
    result.evidence.captureId = observation.captureId;
    result.evidence.analysisId = observation.analysisId;
    result.evidence.observations.push_back(std::move(observation));
    result.expectedCandidates.push_back(std::move(expected));
    result.misleadingRisk = std::move(risk);
    return result;
}

} // namespace

std::vector<RootCauseBenchmarkCase> DefaultRootCauseBenchmarkCases(
    const std::string& captureId, const std::string& analysisId)
{
    std::vector<RootCauseBenchmarkCase> cases;
    if (captureId.empty() || analysisId.empty()) return cases;
    cases.push_back(MakeBenchmarkCase(
        "reset-release", "reset release before symptom", "dut.output",
        "dut.reset", "reset changes can be coincident with normal startup release",
        BenchmarkObservation("dut.reset", captureId, analysisId,
            EvidenceObservationState::Observed, true, false, true, true, true,
            "reset released before the first output divergence", "")));
    cases.push_back(MakeBenchmarkCase(
        "handshake-block", "valid-ready backpressure", "dut.output",
        "dut.ready", "a ready-low interval may be expected backpressure rather than a fault",
        BenchmarkObservation("dut.ready", captureId, analysisId,
            EvidenceObservationState::Observed, true, true, true, true, true,
            "valid remained asserted while ready was low", "")));
    cases.push_back(MakeBenchmarkCase(
        "fifo-control", "FIFO control error", "dut.output",
        "dut.fifo_full", "full/empty flags can lag by one clock when the sampling edge is ambiguous",
        BenchmarkObservation("dut.fifo_full", captureId, analysisId,
            EvidenceObservationState::Observed, true, false, true, true, true,
            "FIFO control flag changed before the output divergence", "")));
    cases.push_back(MakeBenchmarkCase(
        "width-signedness", "width or signedness mismatch", "dut.output",
        "dut.payload", "a truncated or sign-extended value can resemble an upstream arithmetic fault",
        BenchmarkObservation("dut.payload", captureId, analysisId,
            EvidenceObservationState::Observed, true, false, true, true, true,
            "payload changed with a width/sign interpretation mismatch", "")));
    cases.push_back(MakeBenchmarkCase(
        "observation-insufficient", "insufficient observation", "dut.output",
        "dut.hidden", "an unobserved internal signal must not be promoted to a confirmed cause",
        BenchmarkObservation("dut.hidden", captureId, analysisId,
            EvidenceObservationState::Unknown, false, false, false, false, false,
            "", "candidate signal was not captured; add a distinguishing probe")));
    return cases;
}

bool RunRootCauseBenchmark(
    const DependencyGraph& graph,
    const SignalSourceMap& sourceMap,
    const std::vector<RootCauseBenchmarkCase>& cases,
    std::vector<RootCauseBenchmarkResult>& out,
    std::string& error,
    std::size_t maxDepth,
    std::size_t maxCandidates)
{
    error.clear();
    out.clear();
    if (cases.empty() || cases.size() > 64) {
        error = "root-cause benchmark requires 1..64 cases";
        return false;
    }
    out.reserve(cases.size());
    for (const RootCauseBenchmarkCase& testCase : cases) {
        if (testCase.id.empty() || testCase.firstDifferenceSignal.empty() ||
            testCase.evidence.captureId.empty() || testCase.evidence.analysisId.empty()) {
            error = "root-cause benchmark case has incomplete identity: " + testCase.id;
            out.clear();
            return false;
        }
        RootCauseBenchmarkResult result;
        result.id = testCase.id;
        result.label = testCase.label;
        result.expectedCandidates = testCase.expectedCandidates;
        result.misleadingRisk = testCase.misleadingRisk;
        std::string caseError;
        if (!RootCauseAnalyzer::RankWithEvidence(
                testCase.firstDifferenceSignal, graph, sourceMap,
                testCase.evidence, result.ranked, caseError,
                maxDepth, maxCandidates)) {
            error = "benchmark case " + testCase.id + ": " + caseError;
            out.clear();
            return false;
        }
        const std::size_t top = std::min<std::size_t>(3, result.ranked.size());
        for (std::size_t index = 0; index < top && !result.expectedCandidateInTop3; ++index) {
            for (const std::string& expected : testCase.expectedCandidates) {
                if (result.ranked[index].candidate.location.signalName == expected) {
                    result.expectedCandidateInTop3 = true;
                    break;
                }
            }
        }
        result.observationInsufficient = !testCase.evidence.observations.empty();
        for (const auto& observation : testCase.evidence.observations) {
            if (observation.state == EvidenceObservationState::Observed) {
                result.observationInsufficient = false;
                break;
            }
        }
        out.push_back(std::move(result));
    }
    return true;
}

} // namespace debug
} // namespace sigflow
