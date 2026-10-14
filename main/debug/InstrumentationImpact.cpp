#include "InstrumentationImpact.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

namespace sigflow::debug {
namespace {

std::string NormalizeResourceKey(const std::string& input)
{
    std::string output;
    for (const unsigned char c : input) {
        if (std::isalnum(c)) output.push_back(static_cast<char>(std::tolower(c)));
    }
    return output;
}

bool SameStrings(std::vector<std::string> left, std::vector<std::string> right)
{
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

void AddDifference(std::vector<std::string>& target, const std::string& value)
{
    if (std::find(target.begin(), target.end(), value) == target.end()) target.push_back(value);
}

template <typename Map>
bool BuildUniqueResourceMap(const std::vector<InstrumentationResourceMetric>& metrics,
                            Map& output, std::vector<std::string>& blocking,
                            const char* side)
{
    for (const auto& metric : metrics) {
        const std::string normalized = NormalizeResourceKey(metric.key);
        if (normalized.empty()) {
            AddDifference(blocking, std::string(side) + " resource has an empty key");
            continue;
        }
        if (!output.emplace(normalized, &metric).second) {
            AddDifference(blocking, std::string(side) + " resource key collision: " + normalized);
        }
    }
    return blocking.empty();
}

template <typename Map>
bool BuildTimingMap(const std::vector<InstrumentationTimingMetric>& metrics,
                    Map& output, std::vector<std::string>& blocking, const char* side)
{
    for (const auto& metric : metrics) {
        if (metric.domain.empty()) {
            AddDifference(blocking, std::string(side) + " timing domain is empty");
            continue;
        }
        if (!output.emplace(metric.domain, &metric).second) {
            AddDifference(blocking, std::string(side) + " timing domain is duplicated: " + metric.domain);
        }
    }
    return blocking.empty();
}

Json::Value StringsJson(const std::vector<std::string>& values)
{
    Json::Value array(Json::arrayValue);
    for (const auto& value : values) array.append(value);
    return array;
}

} // namespace

const char* ToString(InstrumentationComparability value)
{
    switch (value) {
        case InstrumentationComparability::Comparable: return "comparable";
        case InstrumentationComparability::ObservationOnly: return "observation_only";
        case InstrumentationComparability::NotComparable: return "not_comparable";
    }
    return "not_comparable";
}

const char* ToString(InstrumentationRiskLevel value)
{
    switch (value) {
        case InstrumentationRiskLevel::Info: return "info";
        case InstrumentationRiskLevel::Warning: return "warning";
        case InstrumentationRiskLevel::Blocking: return "blocking";
    }
    return "warning";
}

bool ReadJsonFile(const std::string& path, Json::Value& root, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open instrumentation report: " + path;
        return false;
    }
    Json::CharReaderBuilder reader;
    if (!Json::parseFromStream(reader, input, &root, &error) || !root.isObject()) {
        if (error.empty()) error = "instrumentation report root is not an object: " + path;
        return false;
    }
    return true;
}

bool IsSuccessfulStatus(const Json::Value& root)
{
    if (!root.isMember("status") || !root["status"].isString()) return false;
    const std::string status = root["status"].asString();
    return status == "success" || status == "succeeded" || status == "completed" || status == "ok";
}

void AppendSynthesisMetric(const std::string& key, const Json::Value& value,
                           std::vector<InstrumentationResourceMetric>& out)
{
    if (!value.isInt64() && !value.isUInt64() && !value.isInt() && !value.isUInt()) return;
    InstrumentationResourceMetric metric;
    metric.key = "synth." + key;
    metric.used = value.asUInt64();
    metric.available = true;
    out.push_back(std::move(metric));
}

bool LoadInstrumentationBuildReportFromJson(
    const std::string& synthesisReportPath,
    const std::string& implementationReportPath,
    const InstrumentationBuildIdentity& identity,
    InstrumentationBuildReport& out,
    std::string& error)
{
    error.clear();
    out = InstrumentationBuildReport();
    out.identity = identity;
    out.artifactPath = implementationReportPath;
    out.rawLogPath = synthesisReportPath;
    Json::Value synthesis;
    if (!ReadJsonFile(synthesisReportPath, synthesis, error)) return false;
    Json::Value implementation;
    const bool hasImplementation = !implementationReportPath.empty();
    if (hasImplementation && !ReadJsonFile(implementationReportPath, implementation, error)) return false;

    const Json::Value resources = synthesis["resources"];
    if (resources.isObject()) {
        for (const char* key : {"total_cells", "lut", "dff", "ibuf", "obuf"}) {
            if (resources.isMember(key)) AppendSynthesisMetric(key, resources[key], out.resources);
        }
        if (resources["cell_types"].isArray()) {
            for (const auto& item : resources["cell_types"]) {
                if (item.isObject() && item["type"].isString() && item["count"].isInt()) {
                    AppendSynthesisMetric(item["type"].asString(), item["count"], out.resources);
                }
            }
        }
    }
    bool synthComplete = IsSuccessfulStatus(synthesis);
    if (hasImplementation && implementation["resources"].isObject()) {
        out.resources.clear();
        for (const auto& name : implementation["resources"].getMemberNames()) {
            const auto& item = implementation["resources"][name];
            if (!item.isObject() || !item["used"].isInt() || !item["total"].isInt()) continue;
            InstrumentationResourceMetric metric;
            metric.key = name;
            metric.used = item["used"].asUInt();
            metric.capacity = item["total"].asUInt();
            metric.available = true;
            out.resources.push_back(std::move(metric));
        }
    }
    if (hasImplementation && implementation["timing"].isObject()) {
        const auto& timing = implementation["timing"];
        InstrumentationTimingMetric metric;
        metric.domain = timing.get("clock", "unknown").asString();
        metric.targetMHz = timing.get("target_mhz", 0.0).asDouble();
        metric.achievedMHz = timing.get("max_freq_mhz", 0.0).asDouble();
        metric.passed = timing.get("passed", false).asBool();
        metric.available = !metric.domain.empty() && metric.domain != "unknown" &&
                           timing["max_freq_mhz"].isNumeric();
        metric.evidencePath = implementationReportPath;
        if (metric.available) out.timing.push_back(std::move(metric));
    }
    bool implementationComplete = true;
    if (hasImplementation) {
        implementationComplete = implementation["stages"].isObject() &&
            implementation["stages"].get("route", false).asBool() &&
            implementation.get("errors", 0).asInt() == 0;
    }
    out.completed = synthComplete && implementationComplete;
    return true;
}

bool CompareInstrumentationBuilds(
    const InstrumentationBuildReport& baseline,
    const InstrumentationBuildReport& instrumented,
    const InstrumentationGatePolicy& policy,
    InstrumentationImpactReport& out,
    std::string& error)
{
    error.clear();
    out = InstrumentationImpactReport();
    if (baseline.reportVersion != "1.0" || instrumented.reportVersion != "1.0") {
        error = "unsupported instrumentation report version";
        return false;
    }
    if (!baseline.completed || !instrumented.completed) {
        out.blockingDifferences.push_back("baseline or instrumented build is incomplete");
    }
    const auto& left = baseline.identity;
    const auto& right = instrumented.identity;
    if (left.sourceFingerprint != right.sourceFingerprint) {
        out.sourceChanged = true;
        out.blockingDifferences.push_back("source fingerprint differs");
    }
    if (left.topModule != right.topModule) out.blockingDifferences.push_back("top module differs");
    if (left.targetDevice != right.targetDevice) out.blockingDifferences.push_back("target device differs");
    if (left.speedGrade != right.speedGrade) out.blockingDifferences.push_back("speed grade differs");
    if (left.toolVersion != right.toolVersion) out.blockingDifferences.push_back("tool version differs");
    if (left.synthesisStrategy != right.synthesisStrategy) out.blockingDifferences.push_back("synthesis strategy differs");
    if (left.constraintsFingerprint != right.constraintsFingerprint) {
        out.blockingDifferences.push_back("constraints fingerprint differs");
    }
    if (!SameStrings(left.clockDomains, right.clockDomains)) out.blockingDifferences.push_back("clock domain set differs");
    if (!SameStrings(left.ioSignature, right.ioSignature)) out.blockingDifferences.push_back("functional IO signature differs");
    if (left.placementSeed != right.placementSeed) {
        out.seedChanged = true;
        out.allowedDifferences.push_back("placement seed differs; result is observation-only");
    }

    std::map<std::string, const InstrumentationResourceMetric*> baselineResources;
    std::map<std::string, const InstrumentationResourceMetric*> instrumentedResources;
    BuildUniqueResourceMap(baseline.resources, baselineResources, out.blockingDifferences, "baseline");
    BuildUniqueResourceMap(instrumented.resources, instrumentedResources, out.blockingDifferences, "instrumented");
    std::set<std::string> resourceNames;
    for (const auto& item : baselineResources) resourceNames.insert(item.first);
    for (const auto& item : instrumentedResources) resourceNames.insert(item.first);
    for (const auto& name : resourceNames) {
        InstrumentationResourceDelta delta;
        delta.normalizedName = name;
        const auto base = baselineResources.find(name);
        const auto debug = instrumentedResources.find(name);
        if (base == baselineResources.end() || debug == instrumentedResources.end()) {
            delta.diagnostic = "resource is missing from one side";
            out.resources.push_back(std::move(delta));
            continue;
        }
        const auto& baseMetric = *base->second;
        const auto& debugMetric = *debug->second;
        delta.baselineKey = baseMetric.key;
        delta.instrumentedKey = debugMetric.key;
        delta.baselineUsed = baseMetric.used;
        delta.instrumentedUsed = debugMetric.used;
        delta.capacity = baseMetric.capacity;
        if (!baseMetric.available || !debugMetric.available) {
            delta.diagnostic = "resource report unavailable";
        } else if (baseMetric.capacity != debugMetric.capacity) {
            delta.diagnostic = "resource capacities differ";
            AddDifference(out.blockingDifferences, "resource capacity differs: " + name);
        } else {
            delta.available = true;
            delta.absoluteDelta = static_cast<std::int64_t>(debugMetric.used) -
                                  static_cast<std::int64_t>(baseMetric.used);
            if (baseMetric.used != 0) {
                delta.relativeAvailable = true;
                delta.relativeDelta = static_cast<double>(delta.absoluteDelta) /
                                      static_cast<double>(baseMetric.used);
            }
        }
        out.resources.push_back(std::move(delta));
    }

    std::map<std::string, const InstrumentationTimingMetric*> baselineTiming;
    std::map<std::string, const InstrumentationTimingMetric*> instrumentedTiming;
    BuildTimingMap(baseline.timing, baselineTiming, out.blockingDifferences, "baseline");
    BuildTimingMap(instrumented.timing, instrumentedTiming, out.blockingDifferences, "instrumented");
    std::set<std::string> timingDomains;
    for (const auto& item : baselineTiming) timingDomains.insert(item.first);
    for (const auto& item : instrumentedTiming) timingDomains.insert(item.first);
    for (const auto& domain : timingDomains) {
        InstrumentationTimingDelta delta;
        delta.domain = domain;
        const auto base = baselineTiming.find(domain);
        const auto debug = instrumentedTiming.find(domain);
        if (base == baselineTiming.end() || debug == instrumentedTiming.end()) {
            delta.diagnostic = "timing domain missing from one side";
        } else {
            const auto& baseMetric = *base->second;
            const auto& debugMetric = *debug->second;
            delta.targetMHz = debugMetric.targetMHz != 0.0 ? debugMetric.targetMHz : baseMetric.targetMHz;
            delta.baselineMHz = baseMetric.achievedMHz;
            delta.instrumentedMHz = debugMetric.achievedMHz;
            delta.baselineAvailable = baseMetric.available;
            delta.instrumentedAvailable = debugMetric.available;
            delta.baselinePassed = baseMetric.passed;
            delta.instrumentedPassed = debugMetric.passed;
            if (!delta.baselineAvailable || !delta.instrumentedAvailable) {
                delta.diagnostic = "timing report unavailable";
            } else if (!delta.instrumentedPassed) {
                delta.diagnostic = "instrumented timing failed";
            }
        }
        out.timing.push_back(std::move(delta));
    }

    const bool hardComparable = out.blockingDifferences.empty();
    out.comparability = hardComparable
        ? (out.seedChanged ? InstrumentationComparability::ObservationOnly
                           : InstrumentationComparability::Comparable)
        : InstrumentationComparability::NotComparable;
    if (!hardComparable) {
        out.gate.reasons = out.blockingDifferences;
        out.gate.pass = false;
        return true;
    }

    for (const auto& resource : out.resources) {
        if (!resource.available) {
            out.gate.reasons.push_back("resource report unavailable: " + resource.normalizedName);
            continue;
        }
        if (resource.normalizedName == "lut" || resource.normalizedName == "lut4" ||
            resource.normalizedName == "lut6") {
            if (resource.absoluteDelta > 0 &&
                static_cast<std::uint64_t>(resource.absoluteDelta) > policy.maxAdditionalLut) {
                out.gate.reasons.push_back("LUT increment exceeds policy");
            }
        }
        if (resource.relativeAvailable && resource.relativeDelta > policy.maxAdditionalResourceRatio) {
            out.gate.reasons.push_back("resource increment ratio exceeds policy: " + resource.normalizedName);
        }
    }
    if (policy.requireTimingPass) {
        if (out.timing.empty()) {
            out.gate.reasons.push_back("timing report unavailable: no clock domains");
        }
        for (const auto& timing : out.timing) {
            if (!timing.instrumentedAvailable) {
                out.gate.reasons.push_back("timing report unavailable: " + timing.domain);
            } else if (!timing.instrumentedPassed) {
                out.gate.reasons.push_back("timing failed: " + timing.domain);
            }
        }
    }
    out.gate.pass = out.gate.reasons.empty();
    if (!out.gate.pass && policy.explicitOverrideRequested) {
        if (!policy.allowExplicitOverride) {
            out.gate.reasons.push_back("explicit override requested but policy disallows it");
        } else if (policy.explicitOverrideId.empty() || policy.explicitOverrideReason.empty()) {
            out.gate.reasons.push_back("explicit override requires an id and reason");
        } else {
            out.gate.overridden = true;
            out.gate.overrideId = policy.explicitOverrideId;
            out.gate.overrideReason = policy.explicitOverrideReason;
            out.gate.pass = true;
        }
    }
    return true;
}

std::string GenerateInstrumentationImpactJson(const InstrumentationImpactReport& report)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = report.schemaVersion;
    root["comparability"] = ToString(report.comparability);
    root["source_changed"] = report.sourceChanged;
    root["seed_changed"] = report.seedChanged;
    root["blocking_differences"] = StringsJson(report.blockingDifferences);
    root["allowed_differences"] = StringsJson(report.allowedDifferences);
    Json::Value resources(Json::arrayValue);
    for (const auto& item : report.resources) {
        Json::Value value(Json::objectValue);
        value["normalized_name"] = item.normalizedName;
        value["baseline_key"] = item.baselineKey;
        value["instrumented_key"] = item.instrumentedKey;
        value["baseline_used"] = Json::UInt64(item.baselineUsed);
        value["instrumented_used"] = Json::UInt64(item.instrumentedUsed);
        value["capacity"] = Json::UInt64(item.capacity);
        value["absolute_delta"] = Json::Int64(item.absoluteDelta);
        value["available"] = item.available;
        value["relative_available"] = item.relativeAvailable;
        value["relative_delta"] = item.relativeDelta;
        value["diagnostic"] = item.diagnostic;
        resources.append(value);
    }
    root["resources"] = resources;
    Json::Value timing(Json::arrayValue);
    for (const auto& item : report.timing) {
        Json::Value value(Json::objectValue);
        value["domain"] = item.domain;
        value["target_mhz"] = item.targetMHz;
        value["baseline_mhz"] = item.baselineMHz;
        value["instrumented_mhz"] = item.instrumentedMHz;
        value["baseline_available"] = item.baselineAvailable;
        value["instrumented_available"] = item.instrumentedAvailable;
        value["baseline_passed"] = item.baselinePassed;
        value["instrumented_passed"] = item.instrumentedPassed;
        value["diagnostic"] = item.diagnostic;
        timing.append(value);
    }
    root["timing"] = timing;
    root["gate"] = Json::Value(Json::objectValue);
    root["gate"]["pass"] = report.gate.pass;
    root["gate"]["overridden"] = report.gate.overridden;
    root["gate"]["override_id"] = report.gate.overrideId;
    root["gate"]["override_reason"] = report.gate.overrideReason;
    root["gate"]["reasons"] = StringsJson(report.gate.reasons);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, root) + "\n";
}

void AnalyzeInstrumentationRisks(
    const InstrumentationImpactReport& report,
    const std::vector<InstrumentationRiskHint>& hints,
    std::vector<InstrumentationRisk>& out,
    std::size_t fanoutWarningThreshold,
    std::size_t fanoutBlockingThreshold)
{
    out.clear();
    for (const auto& hint : hints) {
        if (hint.signal.empty()) continue;
        InstrumentationRisk risk;
        risk.signal = hint.signal;
        risk.evidencePath = hint.evidencePath;
        if (hint.fanout >= fanoutBlockingThreshold) {
            risk.level = InstrumentationRiskLevel::Blocking;
            risk.reasons.push_back("fanout exceeds blocking threshold");
        } else if (hint.fanout >= fanoutWarningThreshold) {
            risk.level = InstrumentationRiskLevel::Warning;
            risk.reasons.push_back("fanout exceeds warning threshold");
        }
        if (hint.combinationalObservation) {
            if (risk.level == InstrumentationRiskLevel::Info) risk.level = InstrumentationRiskLevel::Warning;
            risk.reasons.push_back("combinational observation may perturb logic depth");
        }
        if (hint.clockSensitive) {
            if (risk.level == InstrumentationRiskLevel::Info) risk.level = InstrumentationRiskLevel::Warning;
            risk.reasons.push_back("clock-sensitive observation requires implementation evidence");
        }
        if (hint.resetSensitive) {
            if (risk.level == InstrumentationRiskLevel::Info) risk.level = InstrumentationRiskLevel::Warning;
            risk.reasons.push_back("reset-sensitive observation may change release timing");
        }
        if (!hint.samplingLatencyKnown) {
            if (risk.level == InstrumentationRiskLevel::Info) risk.level = InstrumentationRiskLevel::Warning;
            risk.reasons.push_back("sampling latency is unknown");
        }
        risk.evidenceBacked = !risk.reasons.empty() && !hint.evidencePath.empty();
        if (!risk.reasons.empty()) out.push_back(std::move(risk));
    }
    if (report.comparability == InstrumentationComparability::NotComparable ||
        !report.blockingDifferences.empty()) {
        InstrumentationRisk risk;
        risk.signal = "<build>";
        risk.level = InstrumentationRiskLevel::Blocking;
        risk.evidenceBacked = false;
        risk.reasons = report.blockingDifferences;
        if (risk.reasons.empty()) risk.reasons.push_back("build identities are not comparable");
        out.push_back(std::move(risk));
    }
    if (!report.gate.pass && !report.gate.reasons.empty()) {
        InstrumentationRisk risk;
        risk.signal = "<gate>";
        risk.level = InstrumentationRiskLevel::Blocking;
        risk.evidenceBacked = true;
        risk.reasons = report.gate.reasons;
        out.push_back(std::move(risk));
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
        if (left.level != right.level) return static_cast<int>(left.level) > static_cast<int>(right.level);
        return left.signal < right.signal;
    });
}

} // namespace sigflow::debug
