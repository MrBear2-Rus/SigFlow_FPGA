#include "MonitorSummary.h"

#include <json/json.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace sigflow::debug {
namespace {

bool ParsePlan(const Json::Value& value, MonitorPlan& plan, std::string& error)
{
    if (!value.isObject() || !value["monitor_id"].isString() ||
        !value["output_root"].isString() || !value["capture_run_id"].isString() ||
        !value["interval_ms"].isUInt64() || !value["max_runtime_ms"].isUInt64() ||
        !value["max_disk_bytes"].isUInt64()) {
        error = "monitor manifest plan is incomplete";
        return false;
    }
    plan.monitorId = value["monitor_id"].asString();
    plan.outputRoot = value["output_root"].asString();
    plan.captureRunId = value["capture_run_id"].asString();
    plan.intervalMs = value["interval_ms"].asUInt64();
    plan.maxRuntimeMs = value["max_runtime_ms"].asUInt64();
    plan.maxDiskBytes = value["max_disk_bytes"].asUInt64();
    return true;
}

bool ParseEvent(const Json::Value& value, MonitorEvent& event)
{
    if (!value.isObject() || !value["sequence"].isUInt64() ||
        !value["event_id"].isString() || !value["type"].isString() ||
        !value["observed_at"].isString() || !value["monotonic_ms"].isUInt64() ||
        !value["capture_id"].isString() || !value["message"].isString()) return false;
    event.sequence = value["sequence"].asUInt64();
    event.eventId = value["event_id"].asString();
    event.type = value["type"].asString();
    event.observedAtUtc = value["observed_at"].asString();
    event.monotonicMs = value["monotonic_ms"].asUInt64();
    event.captureId = value["capture_id"].asString();
    event.message = value["message"].asString();
    return true;
}

std::uint64_t DirectoryBytes(const std::filesystem::path& root)
{
    std::uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_symlink(ec)) { ec.clear(); continue; }
        if (it->is_regular_file(ec)) {
            const auto size = it->file_size(ec);
            if (!ec && size <= std::numeric_limits<std::uint64_t>::max() - total) total += size;
            ec.clear();
        }
    }
    return total;
}

void LoadIncidents(const std::filesystem::path& monitorRoot,
                   MonitorSummary& summary)
{
    const auto indexPath = monitorRoot / "incidents" / "index.json";
    std::ifstream input(indexPath, std::ios::binary);
    if (!input) return;
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string error;
    if (!Json::parseFromStream(reader, input, &root, &error) ||
        !root.isObject() || !root["entries"].isArray()) {
        summary.diagnostics.push_back("incident index unavailable: " +
                                      (error.empty() ? std::string("invalid JSON") : error));
        return;
    }
    for (const auto& entry : root["entries"]) {
        if (!entry.isObject() || !entry["snapshot_id"].isString() ||
            !entry["path"].isString()) {
            summary.diagnostics.push_back("incident index contains an invalid entry");
            continue;
        }
        MonitorIncidentSummary incident;
        incident.snapshotId = entry["snapshot_id"].asString();
        incident.path = entry["path"].asString();
        std::ifstream snapshot(incident.path, std::ios::binary);
        if (snapshot) {
            Json::Value value;
            std::string snapshotError;
            if (Json::parseFromStream(reader, snapshot, &value, &snapshotError) && value.isObject()) {
                incident.incidentId = value.get("incident_id", "").asString();
                incident.triggerType = value.get("trigger_type", "").asString();
                incident.message = value.get("message", "").asString();
            }
        }
        summary.incidents.push_back(std::move(incident));
    }
}

} // namespace

bool MonitorSummaryService::Load(const std::string& manifestPath,
                                 MonitorSummary& summary,
                                 std::string& error,
                                 std::size_t maxEvents)
{
    error.clear();
    summary = MonitorSummary();
    if (manifestPath.empty()) {
        error = "monitor manifest path is empty";
        return false;
    }
    const std::filesystem::path manifest(manifestPath);
    std::ifstream input(manifest, std::ios::binary);
    if (!input) {
        error = "unable to open monitor manifest: " + manifestPath;
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder reader;
    if (!Json::parseFromStream(reader, input, &root, &error) || !root.isObject() ||
        root.get("schema_version", "").asString() != "1.0" || !root["plan"].isObject()) {
        if (error.empty()) error = "monitor manifest schema is unsupported";
        return false;
    }
    if (!ParsePlan(root["plan"], summary.plan, error)) return false;
    if (!MonitorService::LoadCheckpoint(summary.plan, summary.checkpoint, error)) return false;

    const auto monitorRoot = manifest.parent_path();
    summary.totalBytes = DirectoryBytes(monitorRoot);
    std::uint64_t nextSequence = 0;
    std::string eventError;
    summary.eventLogValid = MonitorService::ValidateEventLog(summary.plan, &nextSequence, eventError);
    if (!summary.eventLogValid && !eventError.empty()) summary.diagnostics.push_back(eventError);
    std::ifstream events(monitorRoot / "events.jsonl", std::ios::binary);
    if (events) {
        std::deque<MonitorEvent> recent;
        std::string line;
        while (std::getline(events, line)) {
            if (line.empty()) continue;
            Json::Value value;
            std::istringstream stream(line);
            std::string parseError;
            MonitorEvent event;
            if (!Json::parseFromStream(reader, stream, &value, &parseError) ||
                !ParseEvent(value, event)) continue;
            recent.push_back(std::move(event));
            while (recent.size() > maxEvents) recent.pop_front();
        }
        summary.recentEvents.assign(recent.begin(), recent.end());
    } else {
        summary.diagnostics.push_back("monitor events.jsonl is missing");
    }
    LoadIncidents(monitorRoot, summary);

    std::string storageError;
    summary.storageSampled = MonitorBackpressureService::Sample(
        summary.plan.outputRoot, summary.storage, storageError);
    if (!summary.storageSampled && !storageError.empty()) summary.diagnostics.push_back(storageError);
    return true;
}

} // namespace sigflow::debug
