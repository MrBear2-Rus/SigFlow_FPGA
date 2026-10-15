#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace sigflow::debug {

enum class InstrumentationComparability {
    Comparable,
    ObservationOnly,
    NotComparable,
};

const char* ToString(InstrumentationComparability value);

struct InstrumentationBuildIdentity {
    std::string sourceFingerprint;
    std::string topModule;
    std::string targetDevice;
    std::string speedGrade;
    std::string toolVersion;
    std::string synthesisStrategy;
    std::string constraintsFingerprint;
    std::string placementSeed;
    std::vector<std::string> clockDomains;
    std::vector<std::string> ioSignature;
};

struct InstrumentationResourceMetric {
    std::string key;
    std::uint64_t used = 0;
    std::uint64_t capacity = 0;
    bool available = false;
};

struct InstrumentationTimingMetric {
    std::string domain;
    double targetMHz = 0.0;
    double achievedMHz = 0.0;
    bool available = false;
    bool passed = false;
    std::string evidencePath;
};

struct InstrumentationBuildReport {
    std::string reportVersion = "1.0";
    InstrumentationBuildIdentity identity;
    bool completed = false;
    std::string artifactPath;
    std::string rawLogPath;
    std::vector<InstrumentationResourceMetric> resources;
    std::vector<InstrumentationTimingMetric> timing;
};

struct InstrumentationResourceDelta {
    std::string normalizedName;
    std::string baselineKey;
    std::string instrumentedKey;
    std::uint64_t baselineUsed = 0;
    std::uint64_t instrumentedUsed = 0;
    std::uint64_t capacity = 0;
    std::int64_t absoluteDelta = 0;
    bool available = false;
    bool relativeAvailable = false;
    double relativeDelta = 0.0;
    std::string diagnostic;
};

struct InstrumentationTimingDelta {
    std::string domain;
    double targetMHz = 0.0;
    double baselineMHz = 0.0;
    double instrumentedMHz = 0.0;
    bool baselineAvailable = false;
    bool instrumentedAvailable = false;
    bool baselinePassed = false;
    bool instrumentedPassed = false;
    std::string diagnostic;
};

struct InstrumentationGatePolicy {
    std::uint64_t maxAdditionalLut = std::numeric_limits<std::uint64_t>::max();
    double maxAdditionalResourceRatio = std::numeric_limits<double>::infinity();
    bool requireTimingPass = true;
    bool allowExplicitOverride = false;
    bool explicitOverrideRequested = false;
    std::string explicitOverrideId;
    std::string explicitOverrideReason;
};

struct InstrumentationGateDecision {
    bool pass = false;
    bool overridden = false;
    std::string overrideId;
    std::string overrideReason;
    std::vector<std::string> reasons;
};

enum class InstrumentationRiskLevel {
    Info,
    Warning,
    Blocking,
};

const char* ToString(InstrumentationRiskLevel value);

struct InstrumentationRiskHint {
    std::string signal;
    std::size_t fanout = 0;
    bool combinationalObservation = false;
    bool clockSensitive = false;
    bool resetSensitive = false;
    bool samplingLatencyKnown = true;
    std::string evidencePath;
};

struct InstrumentationRisk {
    std::string signal;
    InstrumentationRiskLevel level = InstrumentationRiskLevel::Info;
    bool evidenceBacked = false;
    std::vector<std::string> reasons;
    std::string evidencePath;
};

struct InstrumentationImpactReport {
    std::string schemaVersion = "1.0";
    InstrumentationComparability comparability = InstrumentationComparability::NotComparable;
    bool sourceChanged = false;
    bool seedChanged = false;
    std::vector<std::string> blockingDifferences;
    std::vector<std::string> allowedDifferences;
    std::vector<InstrumentationResourceDelta> resources;
    std::vector<InstrumentationTimingDelta> timing;
    InstrumentationGateDecision gate;
};

bool CompareInstrumentationBuilds(
    const InstrumentationBuildReport& baseline,
    const InstrumentationBuildReport& instrumented,
    const InstrumentationGatePolicy& policy,
    InstrumentationImpactReport& out,
    std::string& error);

// Adapt the current wx-free Yosys and nextpnr JSON reports into the common
// impact model.  PnR resources/timing are authoritative when present; the
// synthesis report remains as a namespaced fallback and provenance source.
bool LoadInstrumentationBuildReportFromJson(
    const std::string& synthesisReportPath,
    const std::string& implementationReportPath,
    const InstrumentationBuildIdentity& identity,
    InstrumentationBuildReport& out,
    std::string& error);

void AnalyzeInstrumentationRisks(
    const InstrumentationImpactReport& report,
    const std::vector<InstrumentationRiskHint>& hints,
    std::vector<InstrumentationRisk>& out,
    std::size_t fanoutWarningThreshold = 32,
    std::size_t fanoutBlockingThreshold = 128);

std::string GenerateInstrumentationImpactJson(const InstrumentationImpactReport& report);

} // namespace sigflow::debug
