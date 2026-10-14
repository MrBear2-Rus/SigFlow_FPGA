#include "RootCauseEvidenceReport.h"

#include "DebugArtifactIO.h"

#include <json/json.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace sigflow::debug {
namespace {

void AppendStrings(Json::Value& target, const std::vector<std::string>& values)
{
    target = Json::Value(Json::arrayValue);
    for (const auto& value : values) target.append(value);
}

bool ReadString(const Json::Value& object, const char* key, std::string& out,
               std::string& error, bool required = true)
{
    if (!object.isMember(key)) {
        if (required) error = std::string("root-cause report missing field: ") + key;
        return !required;
    }
    if (!object[key].isString()) {
        error = std::string("root-cause report field is not a string: ") + key;
        return false;
    }
    out = object[key].asString();
    return true;
}

bool ReadUnsigned(const Json::Value& object, const char* key, std::size_t& out,
                  std::string& error)
{
    if (!object.isMember(key) || !object[key].isUInt64()) {
        error = std::string("root-cause report field is not an unsigned integer: ") + key;
        return false;
    }
    const auto value = object[key].asUInt64();
    if (value > std::numeric_limits<std::size_t>::max()) {
        error = std::string("root-cause report integer is too large: ") + key;
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

void AppendLocation(Json::Value& target, const SignalLocation& location)
{
    target["signal"] = location.signalName;
    target["source_path"] = location.sourcePath;
    target["source_line"] = location.sourceLine;
    target["sftree_path"] = location.sftreePath;
    target["canvas_id"] = location.canvasId;
    target["clock_domain"] = location.clockDomain;
    target["fanout"] = Json::UInt64(location.fanout);
    target["is_input"] = location.isInput;
    target["is_probe"] = location.isProbe;
    target["width"] = Json::UInt(location.width);
}

Json::Value CandidateJson(const RootCauseEvidenceCandidate& value)
{
    Json::Value candidate(Json::objectValue);
    AppendLocation(candidate["location"], value.candidate.location);
    candidate["graph_distance"] = Json::UInt64(value.candidate.graphDistance);
    candidate["score"] = value.candidate.score;
    candidate["reason"] = value.candidate.reason;
    candidate["observation"] = ToString(value.observation);
    candidate["evidence_score"] = value.evidenceScore;
    AppendStrings(candidate["supporting_evidence"], value.supportingEvidence);
    AppendStrings(candidate["missing_evidence"], value.missingEvidence);
    return candidate;
}

bool ReadStringArray(const Json::Value& object, const char* key,
                     std::vector<std::string>& out, std::string& error)
{
    if (!object.isMember(key) || !object[key].isArray()) {
        error = std::string("root-cause report field is not an array: ") + key;
        return false;
    }
    out.clear();
    for (const auto& item : object[key]) {
        if (!item.isString()) {
            error = std::string("root-cause report array has a non-string item: ") + key;
            return false;
        }
        out.push_back(item.asString());
    }
    return true;
}

bool ReadLocation(const Json::Value& object, SignalLocation& out, std::string& error)
{
    if (!object.isObject() || !ReadString(object, "signal", out.signalName, error) ||
        out.signalName.empty() || !ReadString(object, "source_path", out.sourcePath, error) ||
        !ReadString(object, "sftree_path", out.sftreePath, error) ||
        !ReadString(object, "canvas_id", out.canvasId, error) ||
        !ReadString(object, "clock_domain", out.clockDomain, error)) {
        if (error.empty()) error = "root-cause report location is invalid";
        return false;
    }
    if (!object.isMember("source_line") || !object["source_line"].isInt() ||
        object["source_line"].asInt() < 0 ||
        !object.isMember("fanout") || !object["fanout"].isUInt64() ||
        !object.isMember("is_input") || !object["is_input"].isBool() ||
        !object.isMember("is_probe") || !object["is_probe"].isBool() ||
        !object.isMember("width") || !object["width"].isUInt()) {
        error = "root-cause report location has invalid metadata";
        return false;
    }
    out.sourceLine = object["source_line"].asInt();
    out.fanout = object["fanout"].asUInt64();
    out.isInput = object["is_input"].asBool();
    out.isProbe = object["is_probe"].asBool();
    out.width = object["width"].asUInt();
    return true;
}

bool ReadCandidate(const Json::Value& item, RootCauseEvidenceCandidate& out,
                   std::string& error)
{
    std::string observation;
    if (!item.isObject() || !item.isMember("location") ||
        !ReadLocation(item["location"], out.candidate.location, error) ||
        !ReadUnsigned(item, "graph_distance", out.candidate.graphDistance, error) ||
        !item.isMember("score") || !item["score"].isNumeric() ||
        !ReadString(item, "reason", out.candidate.reason, error) ||
        !ReadString(item, "observation", observation, error) ||
        !item.isMember("evidence_score") || !item["evidence_score"].isNumeric() ||
        !ReadStringArray(item, "supporting_evidence", out.supportingEvidence, error) ||
        !ReadStringArray(item, "missing_evidence", out.missingEvidence, error)) {
        if (error.empty()) error = "root-cause report candidate is invalid";
        return false;
    }
    if (observation == "observed") out.observation = EvidenceObservationState::Observed;
    else if (observation == "unobserved") out.observation = EvidenceObservationState::Unobserved;
    else if (observation == "unknown") out.observation = EvidenceObservationState::Unknown;
    else {
        error = "root-cause report observation state is unsupported";
        return false;
    }
    out.candidate.score = item["score"].asDouble();
    out.evidenceScore = item["evidence_score"].asDouble();
    return true;
}

} // namespace

bool BuildRootCauseEvidenceReport(
    const RootCauseEvidenceContext& context,
    const std::string& firstDifferenceSignal,
    const std::string& inputFingerprint,
    std::size_t maxDepth,
    std::size_t maxCandidates,
    const std::vector<RootCauseEvidenceCandidate>& candidates,
    RootCauseEvidenceReport& out,
    std::string& error)
{
    error.clear();
    out = RootCauseEvidenceReport();
    if (context.captureId.empty() || context.analysisId.empty()) {
        error = "root-cause report requires capture_id and analysis_id";
        return false;
    }
    if (firstDifferenceSignal.empty()) {
        error = "root-cause report requires first_difference_signal";
        return false;
    }
    if (inputFingerprint.empty()) {
        error = "root-cause report requires input_fingerprint";
        return false;
    }
    if (maxDepth == 0 || maxCandidates == 0) {
        error = "root-cause report requires positive analysis limits";
        return false;
    }
    if (candidates.size() > maxCandidates) {
        error = "root-cause report contains more candidates than max_candidates";
        return false;
    }
    out.captureId = context.captureId;
    out.analysisId = context.analysisId;
    out.firstDifferenceSignal = firstDifferenceSignal;
    out.inputFingerprint = inputFingerprint;
    out.maxDepth = maxDepth;
    out.maxCandidates = maxCandidates;
    out.candidates = candidates;
    return true;
}

std::string GenerateRootCauseEvidenceReportJson(const RootCauseEvidenceReport& report)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = report.schemaVersion;
    root["analyzer_version"] = report.analyzerVersion;
    root["rules_version"] = report.rulesVersion;
    root["capture_id"] = report.captureId;
    root["analysis_id"] = report.analysisId;
    root["first_difference_signal"] = report.firstDifferenceSignal;
    root["input_fingerprint"] = report.inputFingerprint;
    root["max_depth"] = Json::UInt64(report.maxDepth);
    root["max_candidates"] = Json::UInt64(report.maxCandidates);
    root["candidate_count"] = Json::UInt(report.candidates.size());
    Json::Value candidates(Json::arrayValue);
    for (const auto& candidate : report.candidates) candidates.append(CandidateJson(candidate));
    root["candidates"] = candidates;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, root) + "\n";
}

bool SaveRootCauseEvidenceReport(const RootCauseEvidenceReport& report,
                                 const std::string& path,
                                 std::string& error)
{
    if (path.empty()) {
        error = "root-cause report path is empty";
        return false;
    }
    return WriteTextFileAtomic(std::filesystem::path(path),
                               GenerateRootCauseEvidenceReportJson(report), error);
}

bool LoadRootCauseEvidenceReport(const std::string& path,
                                 RootCauseEvidenceReport& out,
                                 std::string& error)
{
    error.clear();
    out = RootCauseEvidenceReport();
    if (path.empty()) {
        error = "root-cause report path is empty";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open root-cause report: " + path;
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder reader;
    if (!Json::parseFromStream(reader, input, &root, &error) || !root.isObject()) {
        if (error.empty()) error = "root-cause report root is not an object";
        return false;
    }
    if (!ReadString(root, "schema_version", out.schemaVersion, error) || out.schemaVersion != "1.0" ||
        !ReadString(root, "analyzer_version", out.analyzerVersion, error) ||
        !ReadString(root, "rules_version", out.rulesVersion, error) ||
        !ReadString(root, "capture_id", out.captureId, error) || out.captureId.empty() ||
        !ReadString(root, "analysis_id", out.analysisId, error) || out.analysisId.empty() ||
        !ReadString(root, "first_difference_signal", out.firstDifferenceSignal, error) ||
        out.firstDifferenceSignal.empty() ||
        !ReadString(root, "input_fingerprint", out.inputFingerprint, error) ||
        out.inputFingerprint.empty() || !ReadUnsigned(root, "max_depth", out.maxDepth, error) ||
        !ReadUnsigned(root, "max_candidates", out.maxCandidates, error) ||
        out.maxDepth == 0 || out.maxCandidates == 0 ||
        !root.isMember("candidate_count") || !root["candidate_count"].isUInt() ||
        !root.isMember("candidates") || !root["candidates"].isArray()) {
        if (error.empty()) error = "root-cause report header is invalid";
        return false;
    }
    if (root["candidate_count"].asUInt64() != root["candidates"].size() ||
        root["candidates"].size() > out.maxCandidates) {
        error = "root-cause report candidate count is inconsistent";
        return false;
    }
    for (const auto& item : root["candidates"]) {
        RootCauseEvidenceCandidate candidate;
        if (!ReadCandidate(item, candidate, error)) return false;
        out.candidates.push_back(std::move(candidate));
    }
    return true;
}

} // namespace sigflow::debug
