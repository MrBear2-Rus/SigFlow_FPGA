#pragma once

#include <eda/api/Types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace eda {
namespace agent {

// SF-05：由 wx/UI 桥提取的不可变工程输入。核心服务不直接访问编辑器或 SigTree。
struct SnapshotSourceInput {
    std::string sourceId;
    std::filesystem::path relativePath;
    // 工程外 include 使用绝对路径，并且必须落在 request.allowedExternalRoots 内。
    // 快照中会改写为 external/<source_id>/<filename>，不记录本机绝对路径。
    std::filesystem::path externalPath;
    // DTO 构造时观察到的内容 hash；非空时 Create 必须命中，防止 IDE 外部修改
    // 在 UI revision 缓存更新前被错误归入旧 revision。
    std::string expectedSha256;
};

struct SnapshotRequest {
    std::string projectId;
    std::filesystem::path projectRoot;
    std::string expectedRevision;
    std::string currentRevision;
    bool dirty = false;
    bool synchronized = true;
    std::string top;
    std::vector<SnapshotSourceInput> sources;
    std::vector<std::filesystem::path> allowedExternalRoots;
    // DTO 构造时的 sigflow.project hash；非空时在收集前后及提交前复核。
    std::string expectedProjectManifestSha256;
    Json target = Json::object();
    Json toolConfig = Json::object();
};

struct SnapshotSource {
    std::string sourceId;
    std::string relativePath;  // 工程相对 POSIX 路径；不暴露绝对路径。
    std::string sha256;
    std::uint64_t size = 0;
    std::string origin = "project";  // project | external
};

struct SnapshotRecord {
    std::string id;
    std::string projectId;
    std::string revision;
    std::string createdAt;
    std::uint64_t createdAtEpoch = 0;
    std::string inputFingerprint;
    std::string top;
    std::vector<SnapshotSource> sources;
    Json target = Json::object();
    Json toolConfig = Json::object();
};

enum class SnapshotError {
    None,
    InvalidArgument,
    StaleRevision,
    DirtyProject,
    SyncInProgress,
    SourceUnavailable,
    PathOutsideProject,
    SourceChanged,
    StorageError,
    NotFound,
};

struct SnapshotResult {
    SnapshotError code = SnapshotError::None;
    std::string message;
    SnapshotRecord snapshot;

    explicit operator bool() const { return code == SnapshotError::None; }
};

class SnapshotService {
public:
    struct Limits {
        std::size_t maxSources = 1000;
        std::uint64_t maxTotalBytes = 64ull * 1024ull * 1024ull;
    };

    // storageRoot 通常为 <project>/.sigflow/agent/snapshots。
    explicit SnapshotService(std::filesystem::path storageRoot);
    SnapshotService(std::filesystem::path storageRoot, Limits limits);

    // revisionProbe 由 GUI 桥提供，用于在读取前和提交前确认 revision 未变化。
    // 核心测试或已持有工程读锁的调用方可以省略。
    SnapshotResult Create(const SnapshotRequest& request,
                          const std::function<std::string()>& revisionProbe = {});

    bool Lookup(const std::string& snapshotId, SnapshotRecord& out, std::string& error) const;
    std::filesystem::path SnapshotDirectory(const std::string& snapshotId) const;

    // 按工程列出全部快照（createdAtEpoch 降序）；projectId 为空列出全部。
    bool List(const std::string& projectId, std::vector<SnapshotRecord>& out,
              std::string& error) const;

    // SF-05：保留工程内最近 keepRecent 个或年龄不超过 maxAgeSeconds 的快照；
    // protectedIds 始终保留。projectId 非空时仅处理该工程的快照（空=全局，旧行为）。
    // nowEpoch=0 时使用当前 UTC 时间。成功时 removed 返回实际删除数量。
    bool Prune(const std::string& projectId, const std::set<std::string>& protectedIds,
               std::size_t keepRecent, std::uint64_t maxAgeSeconds, std::size_t& removed,
               std::string& error, std::uint64_t nowEpoch = 0);
    bool Prune(const std::set<std::string>& protectedIds, std::size_t keepRecent,
               std::uint64_t maxAgeSeconds, std::size_t& removed, std::string& error,
               std::uint64_t nowEpoch = 0);

    static Json ToJson(const SnapshotRecord& snapshot);

private:
    std::filesystem::path storageRoot_;
    Limits limits_;
};

const char* SnapshotErrorCode(SnapshotError error);

} // namespace agent
} // namespace eda
