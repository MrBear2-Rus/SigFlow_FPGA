#pragma once

#include <eda/api/Types.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace eda {
namespace agent {

// SF-02：UI 凭据（receipt）。UI 专用身份签发（hint/L4/教学动作），
// Agent 只能读取与一次性核销（spec §5.2/§5.4）。凭据不进入模型上下文。
struct UiReceipt {
    std::string id;             // ui-receipt-<seq>
    std::string projectId;
    std::string sessionId;
    std::string issue;          // 签发内容摘要（不落入日志/模型上下文）
    std::string level;          // hint | l4 | teaching
    std::string policy;
    std::string revision;
    std::string challengeId;
    std::string actionId;       // 绑定动作（空=不限）
    std::string status = "active";  // active | consumed | invalid
    std::string consumedByRunId;
    std::string consumedActionId;
    std::uint64_t stateVersion = 0;
    std::uint64_t consumedAtEpoch = 0;
    std::string expiresAt;
    std::uint64_t issuedAtEpoch = 0;
    std::uint64_t ttlSeconds = 900;
};

enum class ReceiptDecision {
    Ok,            // 首次核销成功
    Replayed,      // 同 action 重复核销（返回原结果，幂等）
    ActionMismatch,  // 已核销，但核销的是另一个 action（409）
    StateConflict,   // expected_state_version 不符（409）
    NotFound,
    Invalid,       // 已失效（invalid/过期）
    ProjectMismatch,
    StoreUnavailable
};

class ReceiptStore {
public:
    // 配置持久化索引并加载已有 receipt（同 GrantStore：重启后 active 全部失效）。
    // 空路径保留内存模式。损坏时失败关闭。
    bool ConfigurePersistence(const std::filesystem::path& path, std::string& error);

    // 签发（UI 身份在路由层校验）。actionId 可为空（不绑定动作）。
    bool Issue(const std::string& projectId, const std::string& sessionId,
               const std::string& issue, const std::string& level, const std::string& policy,
               const std::string& revision, const std::string& challengeId,
               const std::string& actionId, std::uint64_t ttlSeconds, UiReceipt& out,
               std::string& error);

    bool Lookup(const std::string& receiptId, UiReceipt& out) const;

    // 撤销（UI 专用；路由层校验）；撤销后不可核销。
    bool Invalidate(const std::string& receiptId, std::string& error);

    // 原子核销：
    // - 未失效 + expected_state_version 命中 → 首次扣减，Ok
    // - 已 consumed 且 actionId 相同 → Replayed（幂等重放，返回原结果）
    // - 已 consumed 且 actionId 不同 → ActionMismatch（409）
    // - expected_state_version 不符 → StateConflict（409）
    // - project 归属不符 → ProjectMismatch（403）
    // - 已 invalid/过期 → Invalid（403）
    bool Consume(const std::string& receiptId, const std::string& projectId,
                 const std::string& runId, const std::string& actionId,
                 std::uint64_t expectedStateVersion, ReceiptDecision& decision,
                 UiReceipt& out, std::string& error);

private:
    bool PersistLocked(std::string& error) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, UiReceipt> receipts_;
    std::uint64_t nextId_ = 0;
    std::filesystem::path persistencePath_;
    bool persistenceHealthy_ = true;
};

} // namespace agent
} // namespace eda
