#include "MonitorService.h"

#include "DebugArtifactIO.h"

#include <json/json.h>

#include <chrono>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <limits>

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

std::filesystem::path MonitorRoot(const MonitorPlan& plan)
{
    return std::filesystem::path(plan.outputRoot) / plan.monitorId;
}

std::string JsonString(const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, value) + "\n";
}

std::string JsonLine(const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value) + "\n";
}

Json::Value PlanJson(const MonitorPlan& plan)
{
    Json::Value value(Json::objectValue);
    value["monitor_id"] = plan.monitorId;
    value["output_root"] = plan.outputRoot;
    value["capture_run_id"] = plan.captureRunId;
    value["interval_ms"] = Json::UInt64(plan.intervalMs);
    value["max_runtime_ms"] = Json::UInt64(plan.maxRuntimeMs);
    value["max_disk_bytes"] = Json::UInt64(plan.maxDiskBytes);
    return value;
}

Json::Value CheckpointJson(const MonitorCheckpoint& checkpoint)
{
    Json::Value value(Json::objectValue);
    value["schema_version"] = "1.0";
    value["monitor_id"] = checkpoint.monitorId;
    value["state"] = ToString(checkpoint.state);
    value["next_sequence"] = Json::UInt64(checkpoint.nextSequence);
    value["last_event_id"] = checkpoint.lastEventId;
    value["last_capture_id"] = checkpoint.lastCaptureId;
    value["updated_at"] = checkpoint.updatedAtUtc;
    return value;
}

Json::Value EventJson(const MonitorEvent& event)
{
    Json::Value value(Json::objectValue);
    value["schema_version"] = "1.0";
    value["sequence"] = Json::UInt64(event.sequence);
    value["event_id"] = event.eventId;
    value["type"] = event.type;
    value["observed_at"] = event.observedAtUtc;
    value["monotonic_ms"] = Json::UInt64(event.monotonicMs);
    value["capture_id"] = event.captureId;
    value["message"] = event.message;
    return value;
}

bool IsSafeMonitorId(const std::string& id)
{
    return id.size() > 8 && id.rfind("monitor-", 0) == 0 &&
        id.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}

std::uint64_t DirectoryBytes(const std::filesystem::path& root)
{
    std::uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_symlink(ec)) {
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec)) {
            const auto size = it->file_size(ec);
            if (!ec && size <= std::numeric_limits<std::uint64_t>::max() - total) total += size;
            ec.clear();
        }
    }
    return total;
}

bool ReadCheckpointFile(const std::filesystem::path& path,
                        MonitorCheckpoint& checkpoint,
                        std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open monitor checkpoint: " + path.string();
        return false;
    }
    Json::Value value;
    Json::CharReaderBuilder reader;
    if (!Json::parseFromStream(reader, input, &value, &error) || !value.isObject()) {
        if (error.empty()) error = "monitor checkpoint root is not an object";
        return false;
    }
    if (!value.isMember("schema_version") || value["schema_version"].asString() != "1.0" ||
        !value.isMember("monitor_id") || !value["monitor_id"].isString() ||
        !value.isMember("state") || !value["state"].isString() ||
        !value.isMember("next_sequence") || !value["next_sequence"].isUInt64()) {
        error = "monitor checkpoint header is invalid";
        return false;
    }
    const std::string state = value["state"].asString();
    MonitorState parsed = MonitorState::Failed;
    if (state == "Created") parsed = MonitorState::Created;
    else if (state == "Running") parsed = MonitorState::Running;
    else if (state == "Paused") parsed = MonitorState::Paused;
    else if (state == "Stopped") parsed = MonitorState::Stopped;
    else if (state == "Failed") parsed = MonitorState::Failed;
    else {
        error = "monitor checkpoint state is unknown";
        return false;
    }
    checkpoint.monitorId = value["monitor_id"].asString();
    checkpoint.state = parsed;
    checkpoint.nextSequence = value["next_sequence"].asUInt64();
    checkpoint.lastEventId = value.get("last_event_id", "").asString();
    checkpoint.lastCaptureId = value.get("last_capture_id", "").asString();
    checkpoint.updatedAtUtc = value.get("updated_at", "").asString();
    if (checkpoint.nextSequence == 0) {
        error = "monitor checkpoint next_sequence must be greater than zero";
        return false;
    }
    return true;
}

} // namespace

const char* ToString(MonitorState state)
{
    switch (state) {
        case MonitorState::Created: return "Created";
        case MonitorState::Running: return "Running";
        case MonitorState::Paused: return "Paused";
        case MonitorState::Stopped: return "Stopped";
        case MonitorState::Failed: return "Failed";
    }
    return "Failed";
}

bool MonitorService::ValidatePlan(const MonitorPlan& plan, std::string& error)
{
    if (plan.monitorId.size() <= 8 || plan.monitorId.rfind("monitor-", 0) != 0 ||
        plan.monitorId.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) {
        error = "monitor id must start with monitor- and contain only letters, digits, - or _";
        return false;
    }
    if (plan.outputRoot.empty()) {
        error = "monitor outputRoot is empty";
        return false;
    }
    if (plan.intervalMs == 0) {
        error = "monitor intervalMs must be greater than zero";
        return false;
    }
    return true;
}

bool MonitorService::Create(const MonitorPlan& plan, std::string& error)
{
    error.clear();
    if (!ValidatePlan(plan, error)) return false;
    const auto root = MonitorRoot(plan);
    std::error_code ec;
    std::filesystem::create_directories(root.parent_path(), ec);
    if (ec) {
        error = "unable to create monitor parent: " + ec.message();
        return false;
    }
    if (!std::filesystem::create_directory(root, ec)) {
        error = ec ? "unable to create monitor directory: " + ec.message()
                   : "monitor directory already exists: " + root.string();
        return false;
    }
    Json::Value manifest(Json::objectValue);
    manifest["schema_version"] = "1.0";
    manifest["created_at"] = NowUtc();
    manifest["plan"] = PlanJson(plan);
    if (!WriteTextFileAtomic(root / "monitor.json", JsonString(manifest), error)) return false;
    {
        std::ofstream events(root / "events.jsonl", std::ios::trunc);
        if (!events) {
            error = "unable to create monitor events.jsonl";
            return false;
        }
    }
    MonitorCheckpoint checkpoint;
    checkpoint.monitorId = plan.monitorId;
    checkpoint.updatedAtUtc = NowUtc();
    return SaveCheckpoint(plan, checkpoint, error);
}

bool MonitorService::AppendEvent(const MonitorPlan& plan,
                                 const std::string& type,
                                 const std::string& message,
                                 const std::string& captureId,
                                 std::uint64_t monotonicMs,
                                 MonitorEvent& out,
                                 std::string& error)
{
    error.clear();
    if (!ValidatePlan(plan, error)) return false;
    if (type.empty()) {
        error = "monitor event type is empty";
        return false;
    }
    MonitorCheckpoint checkpoint;
    if (!LoadCheckpoint(plan, checkpoint, error)) return false;
    out = MonitorEvent();
    out.sequence = checkpoint.nextSequence;
    out.eventId = plan.monitorId + "-event-" + std::to_string(out.sequence);
    out.type = type;
    out.observedAtUtc = NowUtc();
    out.monotonicMs = monotonicMs;
    out.captureId = captureId;
    out.message = message;
    const auto path = MonitorRoot(plan) / "events.jsonl";
    std::ofstream events(path, std::ios::app);
    if (!events) {
        error = "unable to append monitor events.jsonl";
        return false;
    }
    events << JsonLine(EventJson(out));
    events.flush();
    if (!events) {
        error = "unable to flush monitor event";
        return false;
    }
    checkpoint.nextSequence = out.sequence + 1;
    checkpoint.lastEventId = out.eventId;
    checkpoint.lastCaptureId = captureId;
    checkpoint.updatedAtUtc = NowUtc();
    if (!SaveCheckpoint(plan, checkpoint, error)) return false;
    return true;
}

bool MonitorService::SaveCheckpoint(const MonitorPlan& plan,
                                    const MonitorCheckpoint& checkpoint,
                                    std::string& error)
{
    error.clear();
    if (!ValidatePlan(plan, error)) return false;
    if (checkpoint.monitorId != plan.monitorId || checkpoint.nextSequence == 0) {
        error = "monitor checkpoint does not match the plan";
        return false;
    }
    MonitorCheckpoint copy = checkpoint;
    if (copy.updatedAtUtc.empty()) copy.updatedAtUtc = NowUtc();
    return WriteTextFileAtomic(MonitorRoot(plan) / "checkpoint.json",
                               JsonString(CheckpointJson(copy)), error);
}

bool MonitorService::TransitionState(const MonitorPlan& plan,
                                     MonitorState nextState,
                                     std::string& error)
{
    error.clear();
    MonitorCheckpoint checkpoint;
    if (!LoadCheckpoint(plan, checkpoint, error)) return false;
    const MonitorState current = checkpoint.state;
    bool allowed = false;
    if (current == MonitorState::Created) {
        allowed = nextState == MonitorState::Running ||
                  nextState == MonitorState::Stopped ||
                  nextState == MonitorState::Failed;
    } else if (current == MonitorState::Running) {
        allowed = nextState == MonitorState::Paused ||
                  nextState == MonitorState::Stopped ||
                  nextState == MonitorState::Failed;
    } else if (current == MonitorState::Paused) {
        allowed = nextState == MonitorState::Running ||
                  nextState == MonitorState::Stopped ||
                  nextState == MonitorState::Failed;
    }
    if (!allowed) {
        error = std::string("invalid monitor state transition from ") +
                ToString(current) + " to " + ToString(nextState);
        return false;
    }
    checkpoint.state = nextState;
    checkpoint.updatedAtUtc.clear();
    return SaveCheckpoint(plan, checkpoint, error);
}

bool MonitorService::LoadCheckpoint(const MonitorPlan& plan,
                                    MonitorCheckpoint& checkpoint,
                                    std::string& error)
{
    error.clear();
    if (!ValidatePlan(plan, error)) return false;
    if (!ReadCheckpointFile(MonitorRoot(plan) / "checkpoint.json", checkpoint, error)) return false;
    if (checkpoint.monitorId != plan.monitorId) {
        error = "monitor checkpoint belongs to another monitor";
        return false;
    }
    return true;
}

bool MonitorService::ValidateEventLog(const MonitorPlan& plan,
                                      std::uint64_t* nextSequence,
                                      std::string& error)
{
    error.clear();
    if (!ValidatePlan(plan, error)) return false;
    std::ifstream input(MonitorRoot(plan) / "events.jsonl", std::ios::binary);
    if (!input) {
        error = "unable to open monitor events.jsonl";
        return false;
    }
    std::uint64_t expected = 1;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        Json::Value value;
        Json::CharReaderBuilder reader;
        std::istringstream stream(line);
        std::string parseError;
        if (!Json::parseFromStream(reader, stream, &value, &parseError) ||
            !value.isObject() || !value["sequence"].isUInt64() ||
            !value["event_id"].isString() || !value["type"].isString()) {
            error = "monitor events.jsonl contains an incomplete or invalid line";
            return false;
        }
        if (value["sequence"].asUInt64() != expected) {
            error = "monitor events.jsonl sequence is not contiguous";
            return false;
        }
        ++expected;
    }
    if (nextSequence) *nextSequence = expected;
    return true;
}

bool MonitorService::BuildRetentionPlan(
    const std::string& outputRoot,
    const MonitorRetentionPolicy& policy,
    const std::vector<std::string>& protectedMonitorIds,
    std::vector<MonitorRetentionAction>& actions,
    std::uint64_t& totalBytes,
    std::string& error)
{
    error.clear();
    actions.clear();
    totalBytes = 0;
    if (outputRoot.empty()) {
        error = "monitor retention outputRoot is empty";
        return false;
    }
    const auto root = std::filesystem::path(outputRoot);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        error = "monitor retention outputRoot is not a directory";
        return false;
    }
    std::unordered_set<std::string> protectedIds(protectedMonitorIds.begin(), protectedMonitorIds.end());
    struct Candidate {
        MonitorRetentionAction action;
        std::filesystem::file_time_type modified;
    };
    std::vector<Candidate> candidates;
    std::uint64_t monitorCount = 0;
    for (std::filesystem::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            error = "unable to enumerate monitor outputRoot: " + ec.message();
            return false;
        }
        if (!it->is_directory(ec) || it->is_symlink(ec)) {
            ec.clear();
            continue;
        }
        const std::string id = it->path().filename().string();
        const std::uint64_t bytes = DirectoryBytes(it->path());
        if (std::numeric_limits<std::uint64_t>::max() - totalBytes < bytes) {
            totalBytes = std::numeric_limits<std::uint64_t>::max();
        } else {
            totalBytes += bytes;
        }
        if (!IsSafeMonitorId(id)) continue; // retained, but still counts toward disk usage
        ++monitorCount;
        if (protectedIds.count(id) != 0) continue;

        MonitorCheckpoint checkpoint;
        std::string checkpointError;
        if (!ReadCheckpointFile(it->path() / "checkpoint.json", checkpoint, checkpointError) ||
            checkpoint.monitorId != id ||
            (checkpoint.state != MonitorState::Stopped && checkpoint.state != MonitorState::Failed)) {
            continue; // active, incomplete, or malformed directories are protected by default
        }
        std::error_code timeError;
        const auto modified = std::filesystem::last_write_time(it->path(), timeError);
        candidates.push_back({{id, bytes, "retention policy"},
                              timeError ? std::filesystem::file_time_type::min() : modified});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  if (left.modified != right.modified) return left.modified < right.modified;
                  return left.action.monitorId < right.action.monitorId;
              });

    std::uint64_t reclaimed = 0;
    std::uint64_t removedCount = 0;
    for (const auto& candidate : candidates) {
        const bool overDisk = policy.maxDiskBytes != 0 &&
            (totalBytes > policy.maxDiskBytes) &&
            (reclaimed < totalBytes - policy.maxDiskBytes);
        const bool overCount = policy.maxMonitorCount != 0 &&
            monitorCount > policy.maxMonitorCount + removedCount;
        if (!overDisk && !overCount) break;
        auto action = candidate.action;
        action.reason = overDisk && overCount ? "disk_budget_and_count" :
                        (overDisk ? "disk_budget" : "monitor_count");
        actions.push_back(std::move(action));
        reclaimed += candidate.action.bytes;
        ++removedCount;
    }
    return true;
}

bool MonitorService::ApplyRetentionPlan(const std::string& outputRoot,
                                        const std::vector<MonitorRetentionAction>& actions,
                                        std::string& error)
{
    error.clear();
    if (outputRoot.empty()) {
        error = "monitor retention outputRoot is empty";
        return false;
    }
    const auto root = std::filesystem::path(outputRoot);
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonicalRoot, ec)) {
        error = "monitor retention outputRoot is not a directory";
        return false;
    }
    for (const auto& action : actions) {
        if (!IsSafeMonitorId(action.monitorId)) {
            error = "monitor retention action has an unsafe monitor id";
            return false;
        }
        const auto dir = root / action.monitorId;
        if (!std::filesystem::is_directory(dir, ec) || std::filesystem::is_symlink(dir, ec)) {
            error = "monitor retention action directory is missing or not a real directory";
            return false;
        }
        const auto canonicalDir = std::filesystem::weakly_canonical(dir, ec);
        if (ec || canonicalDir.parent_path() != canonicalRoot) {
            error = "monitor retention action escapes outputRoot";
            return false;
        }
        MonitorCheckpoint checkpoint;
        if (!ReadCheckpointFile(dir / "checkpoint.json", checkpoint, error) ||
            checkpoint.monitorId != action.monitorId ||
            (checkpoint.state != MonitorState::Stopped && checkpoint.state != MonitorState::Failed)) {
            if (error.empty()) error = "monitor retention action is no longer terminal";
            return false;
        }
        std::filesystem::remove_all(dir, ec);
        if (ec) {
            error = "unable to remove monitor retention directory: " + ec.message();
            return false;
        }
    }
    return true;
}

} // namespace sigflow::debug
