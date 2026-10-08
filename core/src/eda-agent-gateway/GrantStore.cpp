#include "GrantStore.h"

#include "eda-platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace eda {
namespace agent {
namespace {

std::uint64_t NowEpoch() { return static_cast<std::uint64_t>(std::time(nullptr)); }

std::string FormatEpoch(std::uint64_t epoch) {
    const std::time_t value = static_cast<std::time_t>(epoch);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &value);
#else
    gmtime_r(&value, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

bool IsExpired(const Grant& grant, std::uint64_t now) {
    return grant.status == "expired" || now >= grant.issuedAtEpoch + grant.ttlSeconds;
}

Json GrantToJson(const Grant& grant) {
    return Json{{"grant_id", grant.id},
                {"project_id", grant.projectId},
                {"plan_hash", grant.planHash},
                {"revision", grant.revision},
                {"snapshot_id", grant.snapshotId},
                {"status", grant.status},
                {"max_jobs", grant.maxJobs},
                {"used_jobs", grant.usedJobs},
                {"expires_at", grant.expiresAt},
                {"issued_at_epoch", std::to_string(grant.issuedAtEpoch)},
                {"ttl_seconds", std::to_string(grant.ttlSeconds)},
                {"steps", grant.steps}};
}

bool ParseUnsigned(const Json& object, const char* key, std::uint64_t& out) {
    if (!object.contains(key)) return false;
    try {
        if (object[key].is_string()) {
            out = std::stoull(object[key].get<std::string>());
            return true;
        }
        if (object[key].is_number_unsigned()) {
            out = object[key].get<std::uint64_t>();
            return true;
        }
    } catch (const std::exception&) {
    }
    return false;
}

bool ParseGrant(const Json& value, Grant& out) {
    if (!value.is_object()) return false;
    Grant grant;
    grant.id = value.value("grant_id", std::string());
    grant.projectId = value.value("project_id", std::string());
    grant.planHash = value.value("plan_hash", std::string());
    grant.revision = value.value("revision", std::string());
    grant.snapshotId = value.value("snapshot_id", std::string());
    grant.status = value.value("status", std::string());
    grant.maxJobs = value.value("max_jobs", 0);
    grant.usedJobs = value.value("used_jobs", 0);  // 旧文件无该字段时默认 0
    grant.expiresAt = value.value("expires_at", std::string());
    grant.steps = value.value("steps", Json::array());
    if (!ParseUnsigned(value, "issued_at_epoch", grant.issuedAtEpoch) ||
        !ParseUnsigned(value, "ttl_seconds", grant.ttlSeconds)) {
        return false;
    }
    if (grant.id.rfind("grant-", 0) != 0 || grant.projectId.empty() || grant.planHash.empty() ||
        grant.maxJobs <= 0 || grant.usedJobs < 0 || grant.usedJobs > grant.maxJobs ||
        grant.ttlSeconds == 0 || !grant.steps.is_array() || grant.expiresAt.empty() ||
        (grant.status != "active" && grant.status != "revoked" &&
         grant.status != "expired")) {
        return false;
    }
    // 持久化用于审计/恢复核对；新进程或重新打开工程时不得继承自动执行权。
    // 因此所有从磁盘加载的 active grant 都按 expired 恢复。
    if (grant.status == "active") grant.status = "expired";
    out = std::move(grant);
    return true;
}

} // namespace

bool GrantStore::ConfigurePersistence(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    // 工程切换必须先丢弃上一工程的授权。加载失败时保持 unhealthy，
    // 后续签发/撤销均失败关闭，不能退化成未持久化授权。
    grants_.clear();
    nextId_ = 0;
    persistencePath_ = path;
    persistenceHealthy_ = false;
    if (path.empty()) {
        persistenceHealthy_ = true;
        return true;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            error = "unable to inspect grant store: " + ec.message();
            return false;
        }
        persistenceHealthy_ = true;
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to read grant store";
        return false;
    }
    Json document;
    try {
        input >> document;
    } catch (const std::exception&) {
        error = "invalid grant store JSON";
        return false;
    }
    // Windows 不允许替换仍被当前进程打开的文件；过期归一化写回前先释放句柄。
    input.close();
    if (!document.is_object() ||
        document.value("schema_version", std::string()) != "edu.grants.v1" ||
        !document.contains("grants") || !document["grants"].is_array()) {
        error = "unsupported grant store format";
        return false;
    }
    std::uint64_t loadedNextId = 0;
    if (!ParseUnsigned(document, "next_id", loadedNextId)) {
        error = "invalid grant store next_id";
        return false;
    }
    std::unordered_map<std::string, Grant> loaded;
    std::uint64_t maximumGrantSequence = 0;
    bool normalizedExpiry = false;
    for (const auto& item : document["grants"]) {
        Grant grant;
        const std::string storedStatus = item.value("status", std::string());
        if (!ParseGrant(item, grant) || !loaded.emplace(grant.id, grant).second) {
            error = "invalid or duplicate grant record";
            return false;
        }
        try {
            const std::string suffix = grant.id.substr(std::string("grant-").size());
            std::size_t consumed = 0;
            const std::uint64_t sequence = std::stoull(suffix, &consumed);
            if (suffix.empty() || consumed != suffix.size() || sequence == 0) {
                error = "invalid grant id sequence";
                return false;
            }
            maximumGrantSequence = std::max(maximumGrantSequence, sequence);
        } catch (const std::exception&) {
            error = "invalid grant id sequence";
            return false;
        }
        if (storedStatus != grant.status) normalizedExpiry = true;
    }
    if (loadedNextId < maximumGrantSequence) {
        error = "grant store next_id precedes an existing grant";
        return false;
    }
    grants_.swap(loaded);
    nextId_ = loadedNextId;
    persistenceHealthy_ = true;
    if (normalizedExpiry && !PersistLocked(error)) {
        persistenceHealthy_ = false;
        return false;
    }
    return true;
}

Grant GrantStore::Issue(const std::string& projectId, const std::string& planHash,
                        const std::string& revision, const std::string& snapshotId,
                        const Json& steps, std::uint64_t ttlSeconds, int maxJobs) {
    Grant grant;
    std::string error;
    Issue(projectId, planHash, revision, snapshotId, steps, ttlSeconds, maxJobs, grant, error);
    return grant;
}

bool GrantStore::Issue(const std::string& projectId, const std::string& planHash,
                       const std::string& revision, const std::string& snapshotId,
                       const Json& steps, std::uint64_t ttlSeconds, int maxJobs,
                       Grant& out, std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = NowEpoch();
    Grant grant;
    grant.id = "grant-" + std::to_string(++nextId_);
    grant.projectId = projectId;
    grant.planHash = planHash;
    grant.revision = revision;
    grant.snapshotId = snapshotId;
    grant.steps = steps;
    grant.issuedAtEpoch = now;
    grant.ttlSeconds = ttlSeconds == 0 ? 600 : ttlSeconds;
    grant.expiresAt = FormatEpoch(now + grant.ttlSeconds);
    grant.maxJobs = maxJobs <= 0 ? 3 : maxJobs;
    grant.usedJobs = 0;
    grants_[grant.id] = grant;
    if (!PersistLocked(error)) {
        grants_.erase(grant.id);
        --nextId_;
        return false;
    }
    out = grant;
    return true;
}

bool GrantStore::Lookup(const std::string& grantId, Grant& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = grants_.find(grantId);
    if (it == grants_.end()) return false;
    out = it->second;
    if (out.status == "active" && IsExpired(out, NowEpoch())) out.status = "expired";
    return true;
}

std::vector<std::string> GrantStore::ActiveGrantIds(const std::string& projectId) const {
    std::vector<std::string> ids;
    const std::uint64_t now = NowEpoch();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : grants_) {
        if (!projectId.empty() && entry.second.projectId != projectId) continue;
        if (entry.second.status != "active") continue;
        if (IsExpired(entry.second, now)) continue;
        ids.push_back(entry.first);
    }
    return ids;
}

bool GrantStore::Revoke(const std::string& grantId) {
    std::string error;
    return Revoke(grantId, error);
}

bool GrantStore::Revoke(const std::string& grantId, std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = grants_.find(grantId);
    if (it == grants_.end()) return false;
    const std::string previous = it->second.status;
    it->second.status = "revoked";
    if (!PersistLocked(error)) {
        it->second.status = previous;
        return false;
    }
    return true;
}

bool GrantStore::Consume(const std::string& grantId, const std::string& projectId,
                         GrantDecision& decision, std::string& error,
                         const std::string& expectedRevision,
                         const std::string& expectedSnapshotId) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = grants_.find(grantId);
    if (it == grants_.end()) {
        decision = GrantDecision::NotFound;
        error = "unknown grant";
        return false;
    }
    Grant& grant = it->second;
    if (grant.status == "revoked") {
        decision = GrantDecision::Revoked;
        error = "grant is revoked";
        return false;
    }
    if (IsExpired(grant, NowEpoch())) {
        decision = GrantDecision::Expired;
        error = "grant is expired";
        return false;
    }
    if (!projectId.empty() && grant.projectId != projectId) {
        decision = GrantDecision::ProjectMismatch;
        error = "grant belongs to another project";
        return false;
    }
    if ((!expectedRevision.empty() && grant.revision != expectedRevision) ||
        (!expectedSnapshotId.empty() && grant.snapshotId != expectedSnapshotId)) {
        decision = GrantDecision::BindingMismatch;
        error = "grant is not bound to the requested revision and snapshot";
        return false;
    }
    if (grant.usedJobs >= grant.maxJobs) {
        decision = GrantDecision::ProjectExhausted;
        error = "grant job quota is exhausted";
        return false;
    }
    const int previousUsed = grant.usedJobs;
    grant.usedJobs += 1;
    if (!PersistLocked(error)) {
        grant.usedJobs = previousUsed;  // 回滚内存状态
        decision = GrantDecision::Ok;
        return false;
    }
    decision = GrantDecision::Ok;
    return true;
}

GrantDecision GrantStore::Check(const std::string& grantId, const std::string& projectId,
                                Grant* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = grants_.find(grantId);
    if (it == grants_.end()) return GrantDecision::NotFound;
    const Grant& grant = it->second;
    if (out != nullptr) *out = grant;
    if (grant.status == "revoked") return GrantDecision::Revoked;
    if (IsExpired(grant, NowEpoch())) {
        if (out != nullptr) out->status = "expired";
        return GrantDecision::Expired;
    }
    if (!projectId.empty() && grant.projectId != projectId) return GrantDecision::ProjectMismatch;
    return GrantDecision::Ok;
}

bool GrantStore::PersistLocked(std::string& error) const {
    error.clear();
    if (!persistenceHealthy_) {
        error = "grant store is unavailable";
        return false;
    }
    if (persistencePath_.empty()) return true;
    std::error_code ec;
    if (!persistencePath_.parent_path().empty()) {
        std::filesystem::create_directories(persistencePath_.parent_path(), ec);
        if (ec) {
            error = "unable to create grant store directory: " + ec.message();
            return false;
        }
    }

    std::vector<const Grant*> ordered;
    ordered.reserve(grants_.size());
    for (const auto& entry : grants_) ordered.push_back(&entry.second);
    std::sort(ordered.begin(), ordered.end(), [](const Grant* left, const Grant* right) {
        return left->id < right->id;
    });
    Json list = Json::array();
    for (const Grant* grant : ordered) list.push_back(GrantToJson(*grant));
    const Json document{{"schema_version", "edu.grants.v1"},
                        {"next_id", std::to_string(nextId_)},
                        {"grants", list}};

    static std::atomic<std::uint64_t> temporarySequence{0};
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary = std::filesystem::path(persistencePath_).concat(
        ".tmp." + std::to_string(ticks) + "." +
        std::to_string(temporarySequence.fetch_add(1)));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "unable to write grant store temporary file";
            return false;
        }
        output << document.dump(2) << "\n";
        output.flush();
        if (!output) {
            error = "unable to flush grant store temporary file";
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    std::filesystem::rename(temporary, persistencePath_, ec);
    if (ec) {
        std::error_code removeError;
        std::filesystem::remove(persistencePath_, removeError);
        ec.clear();
        std::filesystem::rename(temporary, persistencePath_, ec);
    }
    if (ec) {
        error = "unable to replace grant store: " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

} // namespace agent
} // namespace eda
