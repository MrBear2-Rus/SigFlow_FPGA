#pragma once

#include <eda/api/Types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace eda {
namespace agent {

// NG-05 / SF-07：局部设计证据与未保存缓冲的只读语义。
//
// 边界（不可放宽）：
//   * 本服务只承载宿主在 UI/设计模型线程上抽取出来的**不可变** DTO；HTTP worker 只读快照，
//     不触碰 wx/SigTree，也不直接遍历正在变化的图元。
//   * 未保存编辑缓冲（BufferExcerpt）只用于**解释**：它没有 source_id、不会出现在
//     SnapshotRequest 里，对外一律带 origin="unsaved_buffer" 与 usable_for_execution=false。
//     Gateway 没有任何路径能把缓冲内容变成可执行输入。
//   * 映射不可靠时给出 unavailable/ambiguous/stale，不猜测、不伪造行号或高亮对象；
//     保存/编辑/切工程竞态由 revision 与源码 hash 双重校验兜住，不串版本。
//
// 线程模型：宿主写入口与 HTTP worker 查询入口用同一把锁；写入口整体替换一个工程的映射表
// 或选择上下文，查询侧拿到的是拷贝，不会观察到半更新状态。

enum class MappingStatus {
    Available,
    Ambiguous,
    Stale,
    Unavailable,
};

const char* MappingStatusName(MappingStatus status);

// 指向工程内已登记源码的一段。行号 1 基、包含端点（与 source-ref.schema.json 一致）。
struct DesignSourceRef {
    std::string sourceId;
    std::string path;  // 工程相对 POSIX；不暴露本机绝对路径
    std::string fileHash;
    std::uint64_t startLine = 0;  // 0 = 未知
    std::uint64_t endLine = 0;
    // 宿主明确知道这一段无法可靠对应源码（例如综合优化后别名化）。
    bool unmapped = false;
};

struct DesignPort {
    std::string name;
    std::string direction = "unknown";  // input | output | inout | unknown
    std::uint64_t width = 1;
};

struct DesignNodeRecord {
    std::string nodeId;
    std::string kind = "unknown";  // module | instance | net | port | primitive | unknown
    std::string name;
    std::string moduleName;
    std::string instancePath;
    std::string origin = "schematic";  // schematic | rtl
    std::vector<DesignPort> ports;
    std::vector<DesignSourceRef> sourceRefs;
    // 宿主抽取时已经知道的不确定性；服务只在 revision/源码变化时进一步降级。
    MappingStatus declaredStatus = MappingStatus::Available;
    std::string declaredReason;
    std::vector<std::string> candidates;  // declaredStatus==Ambiguous 时的候选 node_id
    std::string revision;                 // 抽取该映射时的 revision
    bool unsavedOnly = false;             // 映射只存在于编辑缓冲
};

// 未保存编辑缓冲片段。text 只用于只读解释，永不进入 snapshot/工具执行。
struct BufferExcerpt {
    std::string path;        // 工程相对 POSIX（仅作定位标注，不作为执行输入）
    std::string bufferHash;  // 未保存缓冲内容 hash
    std::uint64_t startLine = 0;
    std::uint64_t endLine = 0;
    std::string text;
    bool truncated = false;
};

struct SelectionContext {
    std::string revision;  // 抽取该选择时的 revision
    std::vector<std::string> selectedNodeIds;
    std::vector<DesignSourceRef> sourceRefs;
    std::vector<BufferExcerpt> bufferExcerpts;
    std::string bufferHash;
    bool dirty = false;
    std::uint64_t stateVersion = 0;
};

class DesignContextService {
public:
    struct Limits {
        std::size_t maxNodesPerProject = 4096;
        std::size_t maxBufferExcerpts = 8;
        std::size_t maxBufferExcerptBytes = 64 * 1024;
        std::size_t maxPortsPerNode = 256;
        std::size_t maxSourceRefsPerNode = 32;
    };

    DesignContextService();
    explicit DesignContextService(Limits limits);

    // ---- 宿主写入口（UI/设计模型线程）----
    // 整体替换该工程的映射表（原子；超过限额按插入顺序截断并保留计数）。
    void SetNodes(const std::string& projectId, const std::string& revision,
                  std::vector<DesignNodeRecord> nodes);
    // 整体替换该工程的选择上下文；selection.revision 为空时沿用已登记的 revision。
    void SetSelection(const SelectionContext& selection, const std::string& projectId);
    void ClearProject(const std::string& projectId);
    void ClearAll();

    // 已登记的映射表快照（拷贝）。
    struct ProjectState {
        std::string projectId;
        std::string revision;
        std::vector<DesignNodeRecord> nodes;
        SelectionContext selection;
        bool hasSelection = false;
        bool truncatedNodes = false;
    };
    bool Snapshot(const std::string& projectId, ProjectState& out) const;

    // ---- HTTP worker 只读解析 ----
    struct NodeResolution {
        MappingStatus status = MappingStatus::Unavailable;
        // available | node_not_registered | project_unknown | no_source_mapping |
        // unmapped_path | revision_changed | source_changed | source_missing |
        // unsaved_only | ambiguous_node | declared_unavailable
        std::string reason;
        DesignNodeRecord node;
        std::vector<std::string> candidates;
        bool bufferOnly = false;
    };

    NodeResolution ResolveNode(const std::string& projectId, const std::string& nodeId,
                               const std::string& currentRevision,
                               const std::filesystem::path& projectRoot) const;

    struct SelectionResolution {
        bool found = false;  // 该工程登记过选择上下文
        // ok | no_selection | project_unknown | revision_changed
        std::string reason;
        SelectionContext selection;  // 已按限额裁剪；buffer 文本已截断
        std::vector<NodeResolution> nodes;
        bool bufferAvailable = false;
    };

    SelectionResolution ResolveSelection(const std::string& projectId,
                                         const std::string& currentRevision,
                                         const std::filesystem::path& projectRoot,
                                         bool includeBuffer) const;

    static Json NodeToJson(const std::string& projectId, const std::string& currentRevision,
                           bool dirty, const NodeResolution& resolution);
    static Json BufferExcerptToJson(const BufferExcerpt& excerpt);

private:
    struct ProjectEntry {
        std::string revision;
        std::vector<DesignNodeRecord> nodes;
        SelectionContext selection;
        bool hasSelection = false;
    };

    static bool VerifySourceRef(const DesignSourceRef& ref,
                                const std::filesystem::path& projectRoot, std::string& reason);
    // 调用方必须已持有 mutex_（ResolveSelection 需要在同一次上锁中解析多个节点）。
    NodeResolution ResolveNodeLocked(const ProjectEntry& entry, const std::string& nodeId,
                                     const std::string& currentRevision,
                                     const std::filesystem::path& projectRoot) const;
    BufferExcerpt ClampExcerpt(const BufferExcerpt& excerpt) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ProjectEntry> projects_;
    Limits limits_;
};

} // namespace agent
} // namespace eda
