#pragma once

#include <eda/api/Types.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace eda {
namespace agent {

// SF-02 / SF-03：执行授权（grant）。由 UI 专用身份签发，Agent 只能携带使用/查询。
// 绑定 plan_hash、revision、steps、TTL、max_jobs（spec §5.2、§6.4）。
struct Grant {
    std::string id;
    std::string projectId;
    std::string planHash;
    std::string revision;
    std::string snapshotId;
    std::string status = "active";  // active | revoked | expired
    int maxJobs = 3;
    int usedJobs = 0;  // SF-03：已消耗 Job 数（不得超过 maxJobs）
    std::string expiresAt;  // ISO-8601 UTC
    std::uint64_t issuedAtEpoch = 0;
    std::uint64_t ttlSeconds = 600;
    Json steps = Json::array();
};

enum class GrantDecision {
    Ok, NotFound, Revoked, Expired, ProjectMismatch, ProjectExhausted, BindingMismatch
};

class GrantStore {
public:
    // 配置持久化索引并加载已有 grant。空路径保留内存模式，供最小测试使用。
    // 文件损坏或版本不兼容时失败关闭，不覆盖磁盘内容。
    bool ConfigurePersistence(const std::filesystem::path& path, std::string& error);

    // 签发（UI 身份在路由层校验）。TTL 默认 600s，max_jobs 默认 3（spec §6.4）。
    Grant Issue(const std::string& projectId, const std::string& planHash,
                const std::string& revision, const std::string& snapshotId,
                const Json& steps, std::uint64_t ttlSeconds, int maxJobs);
    bool Issue(const std::string& projectId, const std::string& planHash,
               const std::string& revision, const std::string& snapshotId,
               const Json& steps, std::uint64_t ttlSeconds, int maxJobs,
               Grant& out, std::string& error);

    // 查询。
    bool Lookup(const std::string& grantId, Grant& out) const;

    // 该工程当前仍是 active 状态（未撤销/未过期）的 grant id 列表。
    std::vector<std::string> ActiveGrantIds(const std::string& projectId) const;

    // 撤销（UI 专用；路由层校验）。
    bool Revoke(const std::string& grantId);
    bool Revoke(const std::string& grantId, std::string& error);

    // 执行前校验：存在 + 未撤销 + 未过期 + project 归属一致。
    GrantDecision Check(const std::string& grantId, const std::string& projectId,
                        Grant* out = nullptr) const;

    // SF-03：原子消耗一个 Job 配额。返回 false 表示 grant 不可用或配额已满（out 携带原因）。
    // 与 Check 的区别：Check 只校验状态；Consume 在同一临界区内校验并递增 usedJobs。
    bool Consume(const std::string& grantId, const std::string& projectId,
                 GrantDecision& decision, std::string& error,
                 const std::string& expectedRevision = {},
                 const std::string& expectedSnapshotId = {});

private:
    bool PersistLocked(std::string& error) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Grant> grants_;
    std::uint64_t nextId_ = 0;
    std::filesystem::path persistencePath_;
    bool persistenceHealthy_ = true;
};

} // namespace agent
} // namespace eda
