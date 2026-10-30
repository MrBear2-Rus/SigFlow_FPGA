#pragma once
//
// AgentSelectionContext — 可信选择证据的有界 DTO 与 run 请求投影（纯 C++20）
//
// 设计源：docs/agent/SigFlow-Edu-Agent-GUI-Spec.md §5.2（可信内容与证据）、
//         docs/agent/task.md AG-05（ContextBridge 抽取 immutable DTO）、
//         contracts/edu-agent/v1/agent.openapi.yaml `Selection`。
//
// 为什么需要这一层：
//   * v1 `CreateRunRequest.selection` 是「当前选中的受限证据」（opaque node_id/source_id、
//     行号范围、必要时片段 text）；`question` 只是学生的问题，不是工程证据。
//     之前宿主把 question 交给 selection 时 text 恒为空，等于讲解请求不带任何代码证据。
//   * 宿主（MainFrame）在 UI 线程上拥有工程根目录、编辑器选择区与画布选择的**绝对路径**；
//     Agent 只允许收到 opaque ID 与有界片段。绝对路径、脚本、任意文件内容一律不得上线。
//   * 未保存缓冲只能作为**明确的 buffer excerpt**（origin=buffer）解释，绝不能冒充
//     当前磁盘 source_ref（不能携带 source_id）。
//
// 边界：本文件无 wx、无网络、无线程、无文件 IO、无异常、无静态可变状态；
//       只做「裁剪 + 校验 + 投影」，不读配置、不判断教学状态、不生成后端事实。
#include <cstddef>
#include <cstdint>
#include <string>

namespace agent::gui::selection {

/// v1 合同上限：`selection.text` ≤ 64 KiB（UTF-8 字节，服务端按字节判定）。
inline constexpr std::size_t kMaxSelectionBytes = 64u * 1024u;

/// opaque 证据 ID 的长度上限（与服务端 `_parse_selection` 一致：≤256，且仅 [A-Za-z0-9_-]）。
inline constexpr std::size_t kMaxOpaqueIdLength = 256u;

/// 宿主在 UI 线程上抽取的选择快照（不可变事实的拷贝）。
/// 缺字段就是空/0，绝不猜：没有可靠映射时不填 sourceId，没有画布选择时不填 nodeId。
struct Snapshot
{
    std::string projectId;   ///< 抽取时的工程绑定（必填，否则不投影）
    std::string revision;    ///< 抽取时的 revision（必填；与请求不一致即视为跨版本）
    std::string sourceId;    ///< opaque ID（"source-<hash>"）；仅 dirty=false 且工程内相对路径
    std::string nodeId;      ///< opaque 画布节点 ID（"canvas-<uid>"）
    std::uint64_t startLine = 0;   ///< 1 基、含端点；0 = 未知
    std::uint64_t endLine = 0;
    std::string text;        ///< 有界片段（调用方已裁剪；本层仍会再裁剪一次）
    bool dirty = false;      ///< 片段来自未保存编辑缓冲
    std::string bufferHash;  ///< 未保存缓冲 hash（仅 dirty 有意义；不是磁盘 hash）
    bool truncated = false;  ///< text 是否在抽取时已被裁剪

    bool operator==(const Snapshot&) const = default;
};

/// 允许进入 v1 run `selection` 的字段投影。
/// present=false 时调用方只发送兼容的空 selection（text=""+source_id="ui-selection"）。
struct Resolved
{
    bool present = false;
    std::string sourceId;   ///< 空 = 不声称任何磁盘 source_ref
    std::string nodeId;
    std::uint64_t startLine = 0;
    std::uint64_t endLine = 0;
    std::string text;
    bool dirty = false;     ///< true → origin=buffer（未保存证据）
    bool truncated = false;
    /// present=false 的原因（固定 ASCII 码，不含任何路径或原文）：
    /// "no-selection" | "cross-project" | "stale-revision" | "unsafe-id"。
    std::string omitted;

    bool operator==(const Resolved&) const = default;
};

/// 是否是合法的 opaque 证据 ID：长度 1..256，只允许 [A-Za-z0-9_-]。
/// 因此 `C:\proj\rtl\top.v`、`../outside/top.v`、`rtl/top.v` 都不是合法 ID。
bool SafeOpaqueId(const std::string& id);

/// 是否像本机路径（绝对路径/盘符/UNC/父目录穿越/含目录分隔符）。
/// 用于拒绝"把路径当证据"的输入，不做文件系统访问。
bool LooksLikePath(const std::string& value);

/// 是否像 64 位小写十六进制摘要（sha256）。
bool IsHexDigest(const std::string& value);

/// 是否是凭据/脚本样式文本（只判形态，**不回显原文**）：
///   * `api_key=` / `token:` / `secret=` / `password=` / `authorization:` 等
///     key/value 形式（大小写不敏感，允许键后空白）；
///   * `javascript:` / `data:` / `file:` / `vbscript:` 等脚本或本地协议样式前缀
///     （协议冒号后必须紧跟非空白载荷，避免误伤普通英文 `data: 8`）。
/// 正常的教学术语（如 `always` 块、组合逻辑与默认赋值）不在此列。
bool LooksLikeSensitiveText(const std::string& text);

/// 按 UTF-8 字节上限裁剪，且不切断多字节序列。
/// 返回的字节数 ≤ maxBytes；truncated 表示是否真的裁剪过。
std::string ClampUtf8(const std::string& utf8, std::size_t maxBytes, bool& truncated);

/// 就地规范化快照（在任何绑定校验之前）：
///   * text 按 64 KiB 边界裁剪并置 truncated；
///   * 非法/像路径的 sourceId、nodeId 被清空；
///   * 非 64 位十六进制的 bufferHash 被清空；
///   * dirty=true 时 sourceId 一律清空（未保存内容不得冒充磁盘 source_ref）；
///   * 行号非法（end<start）时两端都归零，不猜行号。
Snapshot Bound(Snapshot snapshot);

/// 投影为可上线的选择证据。
///
/// 规则（安全门禁，不放宽）：
///  1. 快照的 projectId/revision 必须与请求的 projectId/revision 完全一致，
///     否则视为跨工程/跨版本证据 → present=false（omitted 说明原因），text 一律不发。
///  2. dirty=true → 只作为 buffer excerpt：不带 source_id，只带 text/行号/origin=buffer。
///  3. 没有 text 也没有任何 ID → present=false；调用方退回兼容空 selection。
///  4. 任何字段都不携带路径；sourceId/nodeId 必须是 opaque ID。
Resolved Resolve(const Snapshot& snapshot, const std::string& projectId,
                 const std::string& revision);

}  // namespace agent::gui::selection
