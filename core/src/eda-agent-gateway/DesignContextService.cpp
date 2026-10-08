#include "DesignContextService.h"

#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"

#include <algorithm>
#include <utility>

namespace eda {
namespace agent {
namespace {

bool HasUnsafePath(const std::string& relative) {
    if (relative.empty()) return true;
    if (relative.size() >= 2 && relative[1] == ':') return true;  // 盘符
    if (relative[0] == '/' || relative[0] == '\\') return true;   // 绝对路径
    return false;
}

bool HasParentEscape(const std::string& relative) {
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end = relative.find_first_of("/\\", start);
        const std::string part =
            relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

// 把工程相对 POSIX 路径解析到工程根内；越界返回空。此处只做词法包含检查，
// 文件是否存在由调用方区分（source_missing 与 unmapped_path 语义不同）。
std::filesystem::path ResolveInsideProject(const std::filesystem::path& projectRoot,
                                           const std::string& relative) {
    if (HasUnsafePath(relative) || HasParentEscape(relative)) return {};
    // 契约里的路径是 UTF-8；Windows 上必须经 PathFromUtf8 解码，否则中文路径找不到文件。
    const std::filesystem::path candidate =
        projectRoot / platform::PathFromUtf8(relative);
    const std::filesystem::path normalized = candidate.lexically_normal();
    const std::filesystem::path root = projectRoot.lexically_normal();
    auto rootIt = root.begin();
    auto candidateIt = normalized.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == normalized.end() || *rootIt != *candidateIt) return {};
    }
    if (candidateIt == normalized.end()) return {};  // 等于工程根，不是文件
    return normalized;
}

bool IsWithinPath(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

std::string TruncateUtf8Safe(const std::string& text, std::size_t maxBytes, bool& truncated) {
    if (text.size() <= maxBytes) {
        truncated = false;
        return text;
    }
    truncated = true;
    std::size_t cut = maxBytes;
    // 不切断 UTF-8 序列：回退到最后一个完整码点边界。
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

} // namespace

const char* MappingStatusName(MappingStatus status) {
    switch (status) {
        case MappingStatus::Available: return "available";
        case MappingStatus::Ambiguous: return "ambiguous";
        case MappingStatus::Stale: return "stale";
        case MappingStatus::Unavailable: return "unavailable";
    }
    return "unavailable";
}

DesignContextService::DesignContextService() = default;
DesignContextService::DesignContextService(Limits limits) : limits_(limits) {}

void DesignContextService::SetNodes(const std::string& projectId, const std::string& revision,
                                    std::vector<DesignNodeRecord> nodes) {
    if (projectId.empty()) return;
    if (nodes.size() > limits_.maxNodesPerProject) {
        nodes.resize(limits_.maxNodesPerProject);
    }
    for (DesignNodeRecord& node : nodes) {
        if (node.revision.empty()) node.revision = revision;
        if (node.ports.size() > limits_.maxPortsPerNode) node.ports.resize(limits_.maxPortsPerNode);
        if (node.sourceRefs.size() > limits_.maxSourceRefsPerNode) {
            node.sourceRefs.resize(limits_.maxSourceRefsPerNode);
        }
        if (node.candidates.size() > limits_.maxSourceRefsPerNode) {
            node.candidates.resize(limits_.maxSourceRefsPerNode);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectEntry& entry = projects_[projectId];
    entry.revision = revision;
    entry.nodes = std::move(nodes);
}

void DesignContextService::SetSelection(const SelectionContext& selection,
                                        const std::string& projectId) {
    if (projectId.empty()) return;
    SelectionContext clamped = selection;
    if (clamped.bufferExcerpts.size() > limits_.maxBufferExcerpts) {
        clamped.bufferExcerpts.resize(limits_.maxBufferExcerpts);
    }
    for (BufferExcerpt& excerpt : clamped.bufferExcerpts) {
        excerpt = ClampExcerpt(excerpt);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectEntry& entry = projects_[projectId];
    if (clamped.revision.empty()) clamped.revision = entry.revision;
    entry.selection = std::move(clamped);
    entry.hasSelection = true;
}

void DesignContextService::ClearProject(const std::string& projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    projects_.erase(projectId);
}

void DesignContextService::ClearAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    projects_.clear();
}

bool DesignContextService::Snapshot(const std::string& projectId, ProjectState& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = projects_.find(projectId);
    if (it == projects_.end()) return false;
    out.projectId = projectId;
    out.revision = it->second.revision;
    out.nodes = it->second.nodes;
    out.selection = it->second.selection;
    out.hasSelection = it->second.hasSelection;
    out.truncatedNodes = false;
    return true;
}

bool DesignContextService::VerifySourceRef(const DesignSourceRef& ref,
                                          const std::filesystem::path& projectRoot,
                                          std::string& reason) {
    if (ref.unmapped) {
        reason = "no_source_mapping";
        return false;
    }
    if (ref.sourceId.empty() || ref.path.empty() || ref.fileHash.empty()) {
        reason = "no_source_mapping";
        return false;
    }
    const std::filesystem::path absolute = ResolveInsideProject(projectRoot, ref.path);
    if (absolute.empty()) {
        reason = "unmapped_path";
        return false;
    }
    std::error_code error;
    if (!std::filesystem::exists(absolute, error)) {
        reason = "source_missing";
        return false;
    }
    // 符号链接/别名逃逸：解析后的真实路径必须仍在工程内。
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(projectRoot, error);
    if (error || canonical.empty() || !IsWithinPath(canonical, canonicalRoot)) {
        reason = "unmapped_path";
        return false;
    }
    const std::string currentHash = platform::Sha256FileHex(absolute);
    if (currentHash.empty()) {
        reason = "source_missing";
        return false;
    }
    if (currentHash != ref.fileHash) {
        reason = "source_changed";
        return false;
    }
    return true;
}

BufferExcerpt DesignContextService::ClampExcerpt(const BufferExcerpt& excerpt) const {
    BufferExcerpt clamped = excerpt;
    bool truncated = false;
    clamped.text = TruncateUtf8Safe(excerpt.text, limits_.maxBufferExcerptBytes, truncated);
    clamped.truncated = excerpt.truncated || truncated;
    if (clamped.text.size() != excerpt.text.size() && clamped.endLine == 0) {
        clamped.endLine = clamped.startLine;
    }
    return clamped;
}

DesignContextService::NodeResolution DesignContextService::ResolveNode(
    const std::string& projectId, const std::string& nodeId, const std::string& currentRevision,
    const std::filesystem::path& projectRoot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto projectIt = projects_.find(projectId);
    if (projectIt == projects_.end()) {
        NodeResolution resolution;
        resolution.reason = "project_unknown";
        return resolution;
    }
    return ResolveNodeLocked(projectIt->second, nodeId, currentRevision, projectRoot);
}

DesignContextService::NodeResolution DesignContextService::ResolveNodeLocked(
    const ProjectEntry& entry, const std::string& nodeId, const std::string& currentRevision,
    const std::filesystem::path& projectRoot) const {
    NodeResolution resolution;
    bool nodeFound = false;
    for (const DesignNodeRecord& node : entry.nodes) {
        if (node.nodeId == nodeId) {
            resolution.node = node;
            nodeFound = true;
            break;
        }
    }
    if (!nodeFound) {
        resolution.reason = "node_not_registered";
        return resolution;
    }
    if (!currentRevision.empty() && !entry.revision.empty() &&
        entry.revision != currentRevision) {
        resolution.status = MappingStatus::Stale;
        resolution.reason = "revision_changed";
        return resolution;
    }
    if (resolution.node.declaredStatus == MappingStatus::Ambiguous) {
        resolution.status = MappingStatus::Ambiguous;
        resolution.reason = resolution.node.declaredReason.empty() ? "ambiguous_node"
                                                                  : resolution.node.declaredReason;
        resolution.candidates = resolution.node.candidates;
        return resolution;
    }
    if (resolution.node.declaredStatus == MappingStatus::Unavailable) {
        resolution.status = MappingStatus::Unavailable;
        resolution.reason = resolution.node.declaredReason.empty() ? "declared_unavailable"
                                                                  : resolution.node.declaredReason;
        return resolution;
    }
    if (resolution.node.unsavedOnly) {
        // 缓冲版本可用于只读解释，但绝不是可执行输入，也不是磁盘 RTL 映射。
        resolution.status = MappingStatus::Available;
        resolution.reason = "unsaved_only";
        resolution.bufferOnly = true;
        return resolution;
    }
    if (resolution.node.sourceRefs.empty()) {
        resolution.status = MappingStatus::Unavailable;
        resolution.reason = "no_source_mapping";
        return resolution;
    }
    for (const DesignSourceRef& ref : resolution.node.sourceRefs) {
        std::string reason;
        if (!VerifySourceRef(ref, projectRoot, reason)) {
            resolution.status = MappingStatus::Stale;
            resolution.reason = reason;
            return resolution;
        }
    }
    resolution.status = MappingStatus::Available;
    resolution.reason = "available";
    return resolution;
}

DesignContextService::SelectionResolution DesignContextService::ResolveSelection(
    const std::string& projectId, const std::string& currentRevision,
    const std::filesystem::path& projectRoot, bool includeBuffer) const {
    SelectionResolution resolution;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto projectIt = projects_.find(projectId);
    if (projectIt == projects_.end()) {
        resolution.reason = "project_unknown";
        return resolution;
    }
    const ProjectEntry& entry = projectIt->second;
    if (!entry.hasSelection) {
        resolution.reason = "no_selection";
        return resolution;
    }
    resolution.found = true;
    resolution.selection = entry.selection;
    // 缓冲片段只在“确实有未保存编辑且本次显式请求”时才返回；其余情况一律清空。
    if (!includeBuffer || !entry.selection.dirty) resolution.selection.bufferExcerpts.clear();
    resolution.bufferAvailable = includeBuffer && entry.selection.dirty &&
                                 !resolution.selection.bufferExcerpts.empty();

    if (resolution.selection.selectedNodeIds.empty() && resolution.selection.sourceRefs.empty() &&
        resolution.selection.bufferExcerpts.empty()) {
        resolution.reason = "no_selection";
        return resolution;
    }
    const bool revisionMismatch = !currentRevision.empty() && !entry.selection.revision.empty() &&
                                  entry.selection.revision != currentRevision;
    resolution.reason = revisionMismatch ? "revision_changed" : "ok";
    for (const std::string& nodeId : resolution.selection.selectedNodeIds) {
        resolution.nodes.push_back(
            ResolveNodeLocked(entry, nodeId, currentRevision, projectRoot));
    }
    return resolution;
}

Json DesignContextService::NodeToJson(const std::string& projectId,
                                      const std::string& currentRevision, bool dirty,
                                      const NodeResolution& resolution) {
    const DesignNodeRecord& node = resolution.node;
    Json ports = Json::array();
    for (const DesignPort& port : node.ports) {
        ports.push_back(Json{{"name", port.name},
                             {"direction", port.direction},
                             {"width", port.width == 0 ? 1 : port.width}});
    }
    Json sourceRefs = Json::array();
    for (const DesignSourceRef& ref : node.sourceRefs) {
        sourceRefs.push_back(Json{{"source_id", ref.sourceId},
                                  {"path", ref.path},
                                  {"file_hash", ref.fileHash},
                                  {"start_line", ref.startLine},
                                  {"end_line", ref.endLine},
                                  {"unmapped", ref.unmapped}});
    }
    Json candidates = Json::array();
    for (const std::string& candidate : resolution.candidates) candidates.push_back(candidate);

    const std::string mappingRevision = node.revision.empty() ? currentRevision : node.revision;
    Json data{{"project_id", projectId},
              {"revision", currentRevision.empty() ? mappingRevision : currentRevision},
              {"mapping_revision", mappingRevision},
              {"mapping_status", MappingStatusName(resolution.status)},
              {"reason", resolution.reason},
              {"candidates", candidates},
              {"unsaved_only", node.unsavedOnly || resolution.bufferOnly},
              // 设计上下文永远不是可执行输入：只有 SigFlow 签发的 snapshot 才绑定执行。
              {"usable_for_execution", false},
              {"node",
               Json{{"node_id", node.nodeId},
                    {"kind", node.kind},
                    {"name", node.name},
                    {"module_name", node.moduleName},
                    {"instance_path", node.instancePath},
                    {"origin", node.unsavedOnly ? "unsaved_buffer" : node.origin},
                    {"ports", ports},
                    {"source_refs", sourceRefs}}}};
    // dirty 只影响可解释性提示，不影响映射状态本身。
    data["design_dirty"] = dirty;
    return data;
}

Json DesignContextService::BufferExcerptToJson(const BufferExcerpt& excerpt) {
    return Json{{"origin", "unsaved_buffer"},
                {"path", excerpt.path},
                {"buffer_hash", excerpt.bufferHash},
                {"start_line", excerpt.startLine},
                {"end_line", excerpt.endLine},
                {"text", excerpt.text},
                {"truncated", excerpt.truncated},
                // 硬边界：缓冲内容仅供解释，永不作为执行/快照输入。
                {"usable_for_execution", false}};
}

} // namespace agent
} // namespace eda
