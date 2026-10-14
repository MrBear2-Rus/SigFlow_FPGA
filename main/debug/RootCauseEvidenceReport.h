#pragma once

#include "RootCauseGraph.h"

#include <string>
#include <vector>

namespace sigflow::debug {

// Persisted, explainable output of RootCauseAnalyzer::RankWithEvidence.
// The report keeps the identities and rule/input versions needed to decide
// whether a reopened result is still comparable to a later analysis.
struct RootCauseEvidenceReport {
    std::string schemaVersion = "1.0";
    std::string analyzerVersion = "1.0";
    std::string rulesVersion = "1.0";
    std::string captureId;
    std::string analysisId;
    std::string firstDifferenceSignal;
    std::string inputFingerprint;
    std::size_t maxDepth = 4;
    std::size_t maxCandidates = 8;
    std::vector<RootCauseEvidenceCandidate> candidates;
};

bool BuildRootCauseEvidenceReport(
    const RootCauseEvidenceContext& context,
    const std::string& firstDifferenceSignal,
    const std::string& inputFingerprint,
    std::size_t maxDepth,
    std::size_t maxCandidates,
    const std::vector<RootCauseEvidenceCandidate>& candidates,
    RootCauseEvidenceReport& out,
    std::string& error);

std::string GenerateRootCauseEvidenceReportJson(const RootCauseEvidenceReport& report);

bool SaveRootCauseEvidenceReport(const RootCauseEvidenceReport& report,
                                 const std::string& path,
                                 std::string& error);

bool LoadRootCauseEvidenceReport(const std::string& path,
                                 RootCauseEvidenceReport& out,
                                 std::string& error);

} // namespace sigflow::debug
