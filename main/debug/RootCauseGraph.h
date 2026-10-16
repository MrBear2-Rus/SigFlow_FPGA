#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "DebugContract.h"

namespace sigflow {
namespace debug {

struct SignalLocation {
    std::string signalName;
    std::string sourcePath;
    int sourceLine = 0;
    std::string sftreePath;
    std::string canvasId;
    std::string clockDomain;
    std::size_t fanout = 0;
    bool isInput = false;
    bool isProbe = false;
    unsigned width = 0; // optional sidecar metadata; absent means unknown
};

struct RootCauseCandidate {
    SignalLocation location;
    std::size_t graphDistance = 0;
    double score = 0.0;
    std::string reason;
};

enum class EvidenceObservationState {
    Observed,
    Unobserved,
    Unknown,
};

const char* ToString(EvidenceObservationState state);

// Evidence is explicitly tied to one capture and one analysis.  A missing or
// mismatched identity is rejected instead of silently mixing sessions.
struct RootCauseEvidenceObservation {
    std::string signalName;
    std::string captureId;
    std::string analysisId;
    EvidenceObservationState state = EvidenceObservationState::Unknown;
    bool waveformChanged = false;
    bool protocolError = false;
    bool mappingAvailable = false;
    bool timeAligned = false;
    bool dataComplete = false;
    std::vector<std::string> supportingFacts;
    std::vector<std::string> missingConditions;
};

struct RootCauseEvidenceContext {
    std::string captureId;
    std::string analysisId;
    std::vector<RootCauseEvidenceObservation> observations;
};

struct RootCauseEvidenceCandidate {
    RootCauseCandidate candidate;
    EvidenceObservationState observation = EvidenceObservationState::Unknown;
    double evidenceScore = 0.0;
    std::vector<std::string> supportingEvidence;
    std::vector<std::string> missingEvidence;
};

struct RootCauseTraversalResult {
    std::vector<std::pair<std::string, std::size_t>> upstream;
    bool cycleDetected = false;
    bool depthLimited = false;
    bool truncated = false;
    bool cancelled = false;
    std::size_t visited = 0;
};

struct RootCauseProbeSuggestion {
    std::string signal;
    std::string reason;
    std::string kind = "root-cause-evidence";
    std::string clockDomain;
    std::uint32_t width = 0;
    bool widthKnown = false;
    int priority = 0;
    std::vector<std::string> distinguishingCandidates;
};

// Deterministic, board-independent benchmark cases for validating the
// evidence ranking contract. These cases carry their own expected candidate
// names and misleading-risk note so a report can explain both hits and misses.
struct RootCauseBenchmarkCase {
    std::string id;
    std::string label;
    std::string firstDifferenceSignal;
    RootCauseEvidenceContext evidence;
    std::vector<std::string> expectedCandidates;
    std::string misleadingRisk;
};

struct RootCauseBenchmarkResult {
    std::string id;
    std::string label;
    std::vector<RootCauseEvidenceCandidate> ranked;
    std::vector<std::string> expectedCandidates;
    std::string misleadingRisk;
    bool expectedCandidateInTop3 = false;
    bool observationInsufficient = false;
};

class SignalSourceMap;
class DependencyGraph;

std::vector<RootCauseBenchmarkCase> DefaultRootCauseBenchmarkCases(
    const std::string& captureId, const std::string& analysisId);

bool RunRootCauseBenchmark(
    const DependencyGraph& graph,
    const SignalSourceMap& sourceMap,
    const std::vector<RootCauseBenchmarkCase>& cases,
    std::vector<RootCauseBenchmarkResult>& out,
    std::string& error,
    std::size_t maxDepth = 4,
    std::size_t maxCandidates = 8);

class SignalSourceMap {
public:
    void Add(SignalLocation location);
    const SignalLocation* Find(const std::string& signalName) const;
    std::vector<std::string> InputSignals() const;
    bool LoadJson(const std::string& path, std::string& error);
    std::size_t Size() const;
    const std::vector<SignalLocation>& Entries() const { return locations_; }
    const std::string& TopModule() const { return topModule_; }

private:
    std::vector<SignalLocation> locations_;
    std::string topModule_;
};

class DependencyGraph {
public:
    void AddNode(SignalLocation location);
    void AddDependency(const std::string& source, const std::string& dependent);
    bool LoadJson(const std::string& path, std::string& error);
    std::vector<std::pair<std::string, std::size_t>> UpstreamWithDistance(
        const std::string& signalName, std::size_t maxDepth = 4) const;
    bool CollectUpstreamWithBounds(
        const std::string& signalName,
        std::size_t maxDepth,
        std::size_t maxNodes,
        const std::function<bool()>& shouldCancel,
        RootCauseTraversalResult& result,
        std::string& error) const;
    std::vector<std::string> Upstream(const std::string& signalName,
                                      std::size_t maxDepth = 4) const;
    std::size_t NodeCount() const;
    const std::string& TopModule() const { return topModule_; }

private:
    struct Edge { std::string source; std::string dependent; };
    std::vector<SignalLocation> nodes_;
    std::vector<Edge> edges_;
    std::string topModule_;
};

class RootCauseAnalyzer {
public:
    static std::vector<RootCauseCandidate> Rank(
        const std::string& firstDifferenceSignal,
        const DependencyGraph& graph,
        const SignalSourceMap& sourceMap,
        std::size_t maxDepth = 4,
        std::size_t maxCandidates = 8);

    static bool RankWithEvidence(
        const std::string& firstDifferenceSignal,
        const DependencyGraph& graph,
        const SignalSourceMap& sourceMap,
        const RootCauseEvidenceContext& context,
        std::vector<RootCauseEvidenceCandidate>& out,
        std::string& error,
        std::size_t maxDepth = 4,
        std::size_t maxCandidates = 8);

    static bool SuggestProbes(
        const std::string& firstDifferenceSignal,
        const DependencyGraph& graph,
        const SignalSourceMap& sourceMap,
        const RootCauseEvidenceContext& context,
        std::vector<RootCauseProbeSuggestion>& out,
        std::string& error,
        std::size_t maxDepth = 4,
        std::size_t maxSuggestions = 8);
};

} // namespace debug
} // namespace sigflow
