#include "DebugSession.h"

#include "DebugThresholds.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

namespace sigflow {
namespace debug {

namespace {

std::string NowUtc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buffer);
}

bool IsSafeSessionId(const std::string& value)
{
    if (value.empty()) return false;
    for (char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

bool EnsureDirectory(const std::string& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) ||
           std::filesystem::create_directories(path, ec);
}

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in) return std::string();
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

bool WriteJsonFile(const std::string& path, const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const std::string content = Json::writeString(writer, value) + "\n";
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    return static_cast<bool>(out);
}

Json::Value ToJson(const DebugSessionInfo& session)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["session_id"] = session.id;
    root["retry_of"] = session.retryOf;
    root["state"] = ToString(session.state);
    root["created_at"] = session.createdAt;
    root["updated_at"] = session.updatedAt;
    root["exit_code"] = session.exitCode;
    root["build_id"] = session.buildId;
    root["bitstream_sha256"] = session.bitstreamSha256;
    Json::Value versions(Json::arrayValue);
    for (const std::string& version : session.toolVersions) {
        versions.append(version);
    }
    root["tool_versions"] = versions;
    Json::Value transitions(Json::arrayValue);
    for (const DebugSessionTransition& transition : session.transitions) {
        Json::Value item(Json::objectValue);
        item["state"] = ToString(transition.state);
        item["timestamp"] = transition.timestamp;
        item["operator"] = transition.operatorName;
        item["reason"] = transition.reason;
        item["exit_code"] = transition.exitCode;
        transitions.append(item);
    }
    root["transitions"] = transitions;
    return root;
}

bool FromJson(const Json::Value& root, DebugSessionInfo& session, std::string& error)
{
    if (!root.isObject() || !root["session_id"].isString() || !root["state"].isString()) {
        error = "invalid session manifest";
        return false;
    }
    session = DebugSessionInfo();
    session.id = root["session_id"].asString();
    if (!IsSafeSessionId(session.id) ||
        !ParseDebugSessionState(root["state"].asString(), session.state)) {
        error = "invalid session id or state";
        return false;
    }
    session.retryOf = root.get("retry_of", "").asString();
    session.createdAt = root.get("created_at", "").asString();
    session.updatedAt = root.get("updated_at", "").asString();
    session.exitCode = root.get("exit_code", 0).asInt();
    session.buildId = root.get("build_id", "").asString();
    session.bitstreamSha256 = root.get("bitstream_sha256", "").asString();
    for (const Json::Value& item : root["tool_versions"]) {
        if (item.isString()) session.toolVersions.push_back(item.asString());
    }
    for (const Json::Value& item : root["transitions"]) {
        if (!item.isObject()) continue;
        DebugSessionTransition transition;
        if (!ParseDebugSessionState(item.get("state", "").asString(), transition.state)) {
            continue;
        }
        transition.timestamp = item.get("timestamp", "").asString();
        transition.operatorName = item.get("operator", "").asString();
        transition.reason = item.get("reason", "").asString();
        transition.exitCode = item.get("exit_code", 0).asInt();
        session.transitions.push_back(std::move(transition));
    }
    return true;
}

bool ReadManifest(const std::string& path, DebugSessionInfo& session, std::string& error)
{
    const std::string content = ReadFile(path);
    if (content.empty()) {
        error = "unable to read session manifest: " + path;
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string parseErrors;
    if (!reader->parse(content.data(), content.data() + content.size(), &root,
                       &parseErrors)) {
        error = "unable to parse session manifest: " + parseErrors;
        return false;
    }
    return FromJson(root, session, error);
}

} // namespace

const char* ToString(DebugSessionState state)
{
    switch (state) {
    case DebugSessionState::Created: return "Created";
    case DebugSessionState::Validating: return "Validating";
    case DebugSessionState::Building: return "Building";
    case DebugSessionState::Programming: return "Programming";
    case DebugSessionState::Armed: return "Armed";
    case DebugSessionState::Captured: return "Captured";
    case DebugSessionState::Compared: return "Compared";
    case DebugSessionState::Failed: return "Failed";
    case DebugSessionState::TimedOut: return "TimedOut";
    case DebugSessionState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

bool ParseDebugSessionState(const std::string& value, DebugSessionState& state)
{
    for (DebugSessionState candidate :
         { DebugSessionState::Created, DebugSessionState::Validating,
           DebugSessionState::Building, DebugSessionState::Programming,
           DebugSessionState::Armed, DebugSessionState::Captured,
           DebugSessionState::Compared, DebugSessionState::Failed,
           DebugSessionState::TimedOut, DebugSessionState::Cancelled }) {
        if (value == ToString(candidate)) {
            state = candidate;
            return true;
        }
    }
    return false;
}

bool IsTerminalDebugSessionState(DebugSessionState state)
{
    return state == DebugSessionState::Failed ||
           state == DebugSessionState::TimedOut ||
           state == DebugSessionState::Cancelled ||
           state == DebugSessionState::Compared;
}

bool IsLegalDebugTransition(DebugSessionState from, DebugSessionState to)
{
    switch (from) {
    case DebugSessionState::Created:
        return to == DebugSessionState::Validating ||
               to == DebugSessionState::Cancelled ||
               to == DebugSessionState::Failed;
    case DebugSessionState::Validating:
        return to == DebugSessionState::Building ||
               to == DebugSessionState::Cancelled ||
               to == DebugSessionState::Failed;
    case DebugSessionState::Building:
        return to == DebugSessionState::Programming ||
               to == DebugSessionState::Cancelled ||
               to == DebugSessionState::Failed;
    case DebugSessionState::Programming:
        return to == DebugSessionState::Armed ||
               to == DebugSessionState::Cancelled ||
               to == DebugSessionState::Failed;
    case DebugSessionState::Armed:
        return to == DebugSessionState::Captured ||
               to == DebugSessionState::TimedOut ||
               to == DebugSessionState::Cancelled ||
               to == DebugSessionState::Failed;
    case DebugSessionState::Captured:
        return to == DebugSessionState::Compared ||
               to == DebugSessionState::Armed ||   // 允许重新布防
               to == DebugSessionState::Failed;
    case DebugSessionState::Compared:
        return to == DebugSessionState::Armed ||   // 允许新一轮调试
               to == DebugSessionState::Failed;
    default:
        return false; // 终态不可再迁移
    }
}

std::string DebugSessionService::DebugRoot(const std::string& projectPath)
{
    return projectPath + "\\.sigflow\\debug";
}

DebugSessionPaths DebugSessionService::GetPaths(const std::string& projectPath,
                                                const std::string& sessionId)
{
    DebugSessionPaths paths;
    paths.root = DebugRoot(projectPath) + "\\" + sessionId;
    paths.overlay = paths.root + "\\overlay";
    paths.scripts = paths.root + "\\scripts";
    paths.logs = paths.root + "\\logs";
    paths.artifacts = paths.root + "\\artifacts";
    paths.reports = paths.root + "\\reports";
    paths.manifest = paths.root + "\\manifest.json";
    return paths;
}

std::string DebugSessionService::ManifestPathFor(const std::string& projectPath,
                                                 const std::string& sessionId)
{
    return GetPaths(projectPath, sessionId).manifest;
}

bool DebugSessionService::Create(const std::string& projectPath,
                                 const DebugContract& contract,
                                 DebugSessionInfo& session, std::string& error) const
{
    if (projectPath.empty()) {
        error = "project path required";
        return false;
    }
    std::string contractError;
    if (!contract.Validate(contractError)) {
        error = contractError;
        return false;
    }

    session = DebugSessionInfo();
    session.id = contract.sessionId.empty() ? NewDebugSessionId() : contract.sessionId;
    if (!IsSafeSessionId(session.id)) {
        error = "invalid session id";
        return false;
    }
    session.state = DebugSessionState::Created;
    session.createdAt = NowUtc();
    session.updatedAt = session.createdAt;
    session.transitions.push_back(
        { DebugSessionState::Created, session.createdAt, "local", "session created", 0 });

    const DebugSessionPaths paths = GetPaths(projectPath, session.id);
    for (const std::string& directory :
         { paths.root, paths.overlay, paths.scripts, paths.logs, paths.artifacts,
           paths.reports }) {
        if (!EnsureDirectory(directory)) {
            error = "unable to create session directory: " + directory;
            return false;
        }
    }
    if (!WriteJsonFile(paths.manifest, ToJson(session))) {
        error = "unable to write session manifest: " + paths.manifest;
        return false;
    }
    return true;
}

bool DebugSessionService::Load(const std::string& projectPath,
                               const std::string& sessionId,
                               DebugSessionInfo& session, std::string& error) const
{
    if (!IsSafeSessionId(sessionId)) {
        error = "invalid session id";
        return false;
    }
    return ReadManifest(ManifestPathFor(projectPath, sessionId), session, error);
}

bool DebugSessionService::Transition(const std::string& projectPath,
                                     const std::string& sessionId,
                                     DebugSessionState target, const std::string& reason,
                                     int exitCode, std::string& error) const
{
    DebugSessionInfo session;
    if (!Load(projectPath, sessionId, session, error)) return false;
    if (!IsLegalDebugTransition(session.state, target)) {
        error = "illegal debug session transition: " +
                std::string(ToString(session.state)) + " -> " + ToString(target);
        return false;
    }
    session.state = target;
    session.updatedAt = NowUtc();
    session.exitCode = exitCode;
    session.transitions.push_back(
        { target, session.updatedAt, "local", reason, exitCode });
    return WriteJsonFile(ManifestPathFor(projectPath, sessionId), ToJson(session));
}

bool DebugSessionService::Cancel(const std::string& projectPath,
                                 const std::string& sessionId,
                                 const std::string& reason, std::string& error) const
{
    DebugSessionInfo session;
    if (!Load(projectPath, sessionId, session, error)) return false;
    if (!IsLegalDebugTransition(session.state, DebugSessionState::Cancelled)) {
        error = "session cannot be cancelled from state " +
                std::string(ToString(session.state));
        return false;
    }
    return Transition(projectPath, sessionId, DebugSessionState::Cancelled,
                      reason, -1, error);
}

bool DebugSessionService::List(const std::string& projectPath,
                               std::vector<DebugSessionInfo>& sessions,
                               std::string& error) const
{
    sessions.clear();
    const std::string root = DebugRoot(projectPath);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return true;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::string id = entry.path().filename().string();
        DebugSessionInfo session;
        std::string loadError;
        if (Load(projectPath, id, session, loadError)) {
            sessions.push_back(std::move(session));
        }
    }
    return true;
}

std::size_t DebugSessionService::CleanupOldSessions(
    const std::string& projectPath, std::size_t keepCount,
    std::vector<std::string>& removedIds, std::string& error) const
{
    removedIds.clear();
    std::vector<DebugSessionInfo> sessions;
    if (!List(projectPath, sessions, error)) return 0;
    if (sessions.size() <= keepCount) return 0;

    std::sort(sessions.begin(), sessions.end(),
        [](const DebugSessionInfo& left, const DebugSessionInfo& right) {
            return left.id > right.id; // 最新在前
        });

    std::size_t removed = 0;
    for (std::size_t i = keepCount; i < sessions.size(); ++i) {
        const DebugSessionInfo& old = sessions[i];
        if (!IsTerminalDebugSessionState(old.state)) continue; // 活跃会话不清理
        std::error_code ec;
        const DebugSessionPaths paths = GetPaths(projectPath, old.id);
        if (std::filesystem::remove_all(paths.root, ec) > 0) {
            removedIds.push_back(old.id);
            ++removed;
        }
    }
    return removed;
}

} // namespace debug
} // namespace sigflow
