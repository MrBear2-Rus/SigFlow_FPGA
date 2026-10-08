#pragma once

#include <eda/api/Types.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace eda {
namespace agent {

// SF-02：写请求幂等仓储。
// 语义：同键 + 同规范化请求 hash → 返回既有资源；同键 + 不同内容 → 冲突（409）。
// 断电于"登记/执行交界"时进入 RECOVERY_REQUIRED，需人工核对（见 spec §5.6）。
class IdempotencyStore {
public:
    enum class Outcome { Fresh, Replay, Conflict, InFlight };

    struct Entry {
        std::string requestHash;
        std::string resourceId;
        bool completed = false;
    };

    // 查找/登记。返回 Fresh 时 outEntry 为新登记项（调用方随后调 Complete）。
    Outcome Begin(const std::string& scope, const std::string& key,
                  const std::string& requestHash, Entry& outEntry);

    // 标记某键已完成并绑定资源 id。
    bool Complete(const std::string& scope, const std::string& key,
                  const std::string& resourceId);

    // Fresh 请求在产生资源前被拒绝时撤销登记，使修正后的同键请求可以重新执行。
    // 仅删除请求 hash 相同且尚未完成的条目，避免误删并发请求或已提交资源。
    bool Abandon(const std::string& scope, const std::string& key,
                 const std::string& requestHash);

    // 查询既有项（用于恢复核对）。
    bool Lookup(const std::string& scope, const std::string& key, Entry& outEntry) const;

    // Restores a completed, manifest-backed submission after Gateway restart.
    // Conflicting data is rejected so a corrupt manifest cannot overwrite a
    // live idempotency association.
    bool RestoreCompleted(const std::string& scope, const std::string& key,
                         const std::string& requestHash, const std::string& resourceId);

    // 规范化请求 hash（对海量/顺序不敏感的 JSON 做稳定序列化）。
    static std::string HashRequest(const Json& request);

private:
    static std::string MakeKey(const std::string& scope, const std::string& key) {
        return scope + "\n" + key;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace agent
} // namespace eda
