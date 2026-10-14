#include "MonitorIncidentService.h"

#include "DebugArtifactIO.h"

#include <json/json.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace sigflow::debug {
namespace {

std::string NowUtc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::uint64_t CurrentMonotonicMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsSafeToken(const std::string& value)
{
    return !value.empty() && value.size() <= 128 &&
        value.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" ) == std::string::npos;
}

std::string JsonString(const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, value) + "\n";
}

std::string Fingerprint(const MonitorIncidentRequest& request)
{
    // Stable FNV-1a is sufficient here: this is a local deduplication key,
    // not an integrity claim about the referenced artifacts.
    std::string canonical = request.incidentId + "\n" + request.triggerType + "\n" +
                            request.conditionKey + "\n" +
                            request.captureId + "\n" + request.eventId + "\n" +
                            request.buildIdentity + "\n";
    for (const auto& evidence : request.evidence) {
        canonical += evidence.kind + "\n" + evidence.id + "\n" + evidence.path + "\n";
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

struct IndexEntry {
    std::string fingerprint;
    std::string snapshotId;
    std::string path;
    std::uint64_t observedMonotonicMs = 0;
};

bool LoadIndex(const std::filesystem::path& path, std::vector<IndexEntry>& entries,
               std::string& error)
{
    entries.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open incident index: " + path.string();
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder reader;
    if (!Json::parseFromStream(reader, input, &root, &error) || !root.isObject() ||
        root.get("schema_version", "").asString() != "1.0" || !root["entries"].isArray()) {
        if (error.empty()) error = "incident index is invalid";
        return false;
    }
    for (const auto& value : root["entries"]) {
        if (!value.isObject() || !value["fingerprint"].isString() ||
            !value["snapshot_id"].isString() || !value["path"].isString() ||
            !value["observed_monotonic_ms"].isUInt64()) {
            error = "incident index entry is invalid";
            return false;
        }
        entries.push_back({value["fingerprint"].asString(), value["snapshot_id"].asString(),
                           value["path"].asString(), value["observed_monotonic_ms"].asUInt64()});
    }
    return true;
}

bool SaveIndex(const std::filesystem::path& path, const std::vector<IndexEntry>& entries,
               std::string& error)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    Json::Value array(Json::arrayValue);
    for (const auto& entry : entries) {
        Json::Value value(Json::objectValue);
        value["fingerprint"] = entry.fingerprint;
        value["snapshot_id"] = entry.snapshotId;
        value["path"] = entry.path;
        value["observed_monotonic_ms"] = Json::UInt64(entry.observedMonotonicMs);
        array.append(value);
    }
    root["entries"] = array;
    return WriteTextFileAtomic(path, JsonString(root), error);
}

Json::Value EvidenceJson(const IncidentEvidenceRef& evidence)
{
    Json::Value value(Json::objectValue);
    value["kind"] = evidence.kind;
    value["id"] = evidence.id;
    value["path"] = evidence.path;
    std::error_code ec;
    value["available"] = !evidence.path.empty() && std::filesystem::is_regular_file(evidence.path, ec);
    return value;
}

Json::Value SnapshotJson(const MonitorIncidentRequest& request,
                         const MonitorIncidentResult& result,
                         const std::string& observedAt)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["snapshot_id"] = result.snapshotId;
    root["incident_id"] = request.incidentId;
    root["trigger_type"] = request.triggerType;
    root["condition_key"] = request.conditionKey;
    root["message"] = request.message;
    root["event_id"] = request.eventId;
    root["capture_id"] = request.captureId;
    root["build_identity"] = request.buildIdentity;
    root["observed_at"] = observedAt;
    root["observed_monotonic_ms"] = Json::UInt64(request.observedMonotonicMs);
    root["fingerprint"] = result.fingerprint;
    Json::Value evidence(Json::arrayValue);
    for (const auto& item : request.evidence) evidence.append(EvidenceJson(item));
    root["evidence"] = evidence;
    return root;
}

} // namespace

bool MonitorIncidentService::Create(const MonitorPlan& plan,
                                    const MonitorIncidentRequest& request,
                                    const MonitorIncidentPolicy& policy,
                                    MonitorIncidentResult& result,
                                    std::string& error)
{
    error.clear();
    result = MonitorIncidentResult();
    if (!IsSafeToken(request.incidentId)) {
        error = "incident id must contain only letters, digits, - or _";
        return false;
    }
    if (request.triggerType.empty() || request.conditionKey.empty()) {
        error = "incident trigger type and condition key are required";
        return false;
    }
    MonitorCheckpoint checkpoint;
    if (!MonitorService::LoadCheckpoint(plan, checkpoint, error)) return false;
    if (checkpoint.state == MonitorState::Created) {
        error = "incident snapshot requires a started monitor";
        return false;
    }

    const auto monitorRoot = std::filesystem::path(plan.outputRoot) / plan.monitorId;
    const auto incidentRoot = monitorRoot / "incidents";
    const auto indexPath = incidentRoot / "index.json";
    std::vector<IndexEntry> entries;
    if (!LoadIndex(indexPath, entries, error)) return false;

    result.fingerprint = Fingerprint(request);
    const std::uint64_t observedMs = request.observedMonotonicMs == 0
                                         ? CurrentMonotonicMs()
                                         : request.observedMonotonicMs;
    for (const auto& entry : entries) {
        if (entry.fingerprint != result.fingerprint) continue;
        if (policy.cooldownMs != 0 && observedMs >= entry.observedMonotonicMs &&
            observedMs - entry.observedMonotonicMs < policy.cooldownMs) {
            result.deduplicated = true;
            result.snapshotId = entry.snapshotId;
            result.snapshotPath = entry.path;
            result.diagnostics.push_back("incident condition suppressed by cooldown");
            MonitorEvent event;
            std::string eventError;
            if (!MonitorService::AppendEvent(plan, "incident.deduplicated",
                                              "incident condition already has a recent snapshot",
                                              request.captureId, observedMs, event, eventError)) {
                error = eventError;
                return false;
            }
            return true;
        }
    }
    if (policy.maxSnapshots != 0 && entries.size() >= policy.maxSnapshots) {
        result.rateLimited = true;
        result.diagnostics.push_back("incident snapshot quota reached");
        MonitorEvent event;
        std::string eventError;
        if (!MonitorService::AppendEvent(plan, "incident.suppressed",
                                          "incident snapshot quota reached", request.captureId,
                                          observedMs, event, eventError)) {
            error = eventError;
            return false;
        }
        return true;
    }

    result.snapshotId = "snapshot-" + std::to_string(entries.size() + 1);
    const auto snapshotPath = incidentRoot / (request.incidentId + "-" + result.snapshotId + ".json");
    result.snapshotPath = snapshotPath.string();
    const std::string observedAt = NowUtc();
    MonitorIncidentRequest snapshotRequest = request;
    snapshotRequest.observedMonotonicMs = observedMs;
    if (!WriteTextFileAtomic(snapshotPath,
                             JsonString(SnapshotJson(snapshotRequest, result, observedAt)), error)) {
        result = MonitorIncidentResult();
        return false;
    }
    entries.push_back({result.fingerprint, result.snapshotId, result.snapshotPath, observedMs});
    if (!SaveIndex(indexPath, entries, error)) {
        std::error_code ignored;
        std::filesystem::remove(snapshotPath, ignored);
        result = MonitorIncidentResult();
        return false;
    }
    MonitorEvent event;
    if (!MonitorService::AppendEvent(plan, "incident.created",
                                     request.message.empty() ? request.triggerType : request.message,
                                     request.captureId, observedMs, event, error)) {
        entries.pop_back();
        std::string rollbackError;
        SaveIndex(indexPath, entries, rollbackError);
        std::error_code ignored;
        std::filesystem::remove(snapshotPath, ignored);
        return false;
    }
    result.created = true;
    return true;
}

} // namespace sigflow::debug
