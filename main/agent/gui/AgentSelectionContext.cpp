// AgentSelectionContext.cpp — 可信选择证据的有界 DTO 与 run 请求投影
// （纯 C++20：无 wxWidgets、无网络、无线程、无文件 IO、无异常、无静态可变状态）
//
// 合同事实（不猜、不放大）：
//   * `Selection.text` 上限 64 KiB（UTF-8 字节；服务端 `len(text.encode("utf-8"))`）。
//   * `Selection.source_id` / `node_id`：服务端要求 ≤256 且只允许 [A-Za-z0-9_-]，
//     否则整条 run 请求 400 INVALID_ARGUMENT。宁可少发一个 ID，也不发非法请求。
//   * `Selection` 里没有任何路径字段：Evidence 只能是 opaque ID + 有界片段。
//     未保存缓冲在 v1 里没有独立字段，因此用最小兼容扩展 `origin: "buffer"` 表达
//     （见 contracts/edu-agent/v1/agent.openapi.yaml），并**不带** source_id。
//
// 确定性：所有决策只依赖入参；输入相同则输出完全相同（不读时钟/配置/文件）。
#include "agent/gui/AgentSelectionContext.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace agent::gui::selection {
namespace {

bool IsOpaqueByte(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
}

bool IsLowerHex(const std::string& value)
{
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

void NormalizeLineRange(std::uint64_t& startLine, std::uint64_t& endLine)
{
    // 合同里 start_line/end_line 是 0 基可选的整数；行号未知用 0 表示"没有证据"。
    // 任何 end<start 的组合都是宿主抽取错误，不猜，直接归零。
    if (startLine == 0 || endLine == 0 || endLine < startLine)
    {
        startLine = 0;
        endLine = 0;
    }
}

}  // namespace

bool SafeOpaqueId(const std::string& id)
{
    if (id.empty() || id.size() > kMaxOpaqueIdLength) return false;
    for (const unsigned char byte : id)
    {
        if (!IsOpaqueByte(byte)) return false;
    }
    return true;
}

bool LooksLikePath(const std::string& value)
{
    if (value.empty()) return false;
    const auto at = [&value](std::size_t index) {
        return static_cast<unsigned char>(value[index]);
    };
    if (at(0) == '/' || at(0) == '\\') return true;            // POSIX 绝对路径 / UNC
    if (value.size() >= 2 && at(1) == ':' &&
        std::isalpha(at(0)) != 0) return true;                  // 盘符绝对路径
    if (value.find("..") != std::string::npos) return true;     // 父目录穿越
    if (value.find('/') != std::string::npos ||
        value.find('\\') != std::string::npos) return true;     // 任何目录分隔符
    return false;
}

bool IsHexDigest(const std::string& value)
{
    return value.size() == 64u && IsLowerHex(value);
}

namespace {

char AsciiLower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// 大小写不敏感的查找（ASCII）；找不到返回 npos。
std::size_t FindIgnoreCase(const std::string& text, const char* needle, std::size_t from)
{
    const std::size_t length = std::strlen(needle);
    if (length == 0 || text.size() < length) return std::string::npos;
    for (std::size_t index = from; index + length <= text.size(); ++index)
    {
        std::size_t offset = 0;
        while (offset < length && AsciiLower(text[index + offset]) == AsciiLower(needle[offset]))
        {
            ++offset;
        }
        if (offset == length) return index;
    }
    return std::string::npos;
}

}  // namespace

bool LooksLikeSensitiveText(const std::string& text)
{
    if (text.empty()) return false;
    static const char* const kSchemes[] = {"javascript:", "data:", "file:", "vbscript:"};
    for (const char* scheme : kSchemes)
    {
        std::size_t pos = 0;
        while ((pos = FindIgnoreCase(text, scheme, pos)) != std::string::npos)
        {
            const std::size_t after = pos + std::strlen(scheme);
            const bool boundaryBefore =
                pos == 0 ||
                (std::isalnum(static_cast<unsigned char>(text[pos - 1])) == 0 &&
                 text[pos - 1] != '_' && text[pos - 1] != '-');
            const bool hasPayload = after < text.size() &&
                std::isspace(static_cast<unsigned char>(text[after])) == 0;
            if (boundaryBefore && hasPayload) return true;
            pos = after;
        }
    }
    static const char* const kCredentialKeys[] = {
        "api_key", "apikey", "access_token", "token", "secret", "password", "passwd",
        "authorization", "auth_key"};
    for (const char* key : kCredentialKeys)
    {
        const std::size_t length = std::strlen(key);
        std::size_t pos = 0;
        while ((pos = FindIgnoreCase(text, key, pos)) != std::string::npos)
        {
            const bool boundaryBefore =
                pos == 0 ||
                (std::isalnum(static_cast<unsigned char>(text[pos - 1])) == 0 &&
                 text[pos - 1] != '_');
            std::size_t next = pos + length;
            while (next < text.size() &&
                   (text[next] == ' ' || text[next] == '\t' || text[next] == '\r'))
            {
                ++next;
            }
            if (boundaryBefore && next < text.size() &&
                (text[next] == '=' || text[next] == ':'))
            {
                return true;
            }
            pos += length;
        }
    }
    return false;
}

std::string ClampUtf8(const std::string& utf8, std::size_t maxBytes, bool& truncated)
{
    if (utf8.size() <= maxBytes)
    {
        truncated = false;
        return utf8;
    }
    std::size_t cut = maxBytes;
    // 退到多字节序列的起始字节，绝不留下半个码点（服务端按 UTF-8 解码）。
    while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0u) == 0x80u)
    {
        --cut;
    }
    truncated = true;
    return utf8.substr(0, cut);
}

Snapshot Bound(Snapshot snapshot)
{
    // 控制符脱敏：代码片段允许换行/回车/制表符，其它 C0 控制符与 DEL 一律替换为空格。
    // 这样请求里既不会出现 NUL/终端控制序列，也不需要丢弃整段证据。
    for (char& ch : snapshot.text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if ((byte < 0x20u && ch != '\n' && ch != '\r' && ch != '\t') || byte == 0x7Fu)
        {
            ch = ' ';
        }
    }
    bool clamped = false;
    std::string text = ClampUtf8(snapshot.text, kMaxSelectionBytes, clamped);
    snapshot.text = std::move(text);
    snapshot.truncated = snapshot.truncated || clamped;

    if (!SafeOpaqueId(snapshot.sourceId)) snapshot.sourceId.clear();
    if (!SafeOpaqueId(snapshot.nodeId)) snapshot.nodeId.clear();
    if (!IsHexDigest(snapshot.bufferHash)) snapshot.bufferHash.clear();
    // 未保存缓冲不得声称任何磁盘 source_ref：即使宿主误填了 sourceId 也在这里清掉。
    if (snapshot.dirty) snapshot.sourceId.clear();
    NormalizeLineRange(snapshot.startLine, snapshot.endLine);
    return snapshot;
}

Resolved Resolve(const Snapshot& snapshot, const std::string& projectId,
                 const std::string& revision)
{
    Resolved resolved;

    const Snapshot bounded = Bound(snapshot);
    const bool hasEvidence = !bounded.text.empty() || !bounded.sourceId.empty() ||
                             !bounded.nodeId.empty();
    if (bounded.projectId.empty() && bounded.revision.empty() && !hasEvidence)
    {
        // 宿主从未抽取过选择：不是错误，只是没有证据可发。
        resolved.omitted = "no-selection";
        return resolved;
    }
    if (bounded.projectId != projectId)
    {
        // 跨工程：另一工程的代码/节点绝不能进入本次请求。
        resolved.omitted = "cross-project";
        return resolved;
    }
    if (bounded.revision != revision)
    {
        // 旧/新 revision 的证据不能绑到当前请求上（保存后旧选择自动失效）。
        resolved.omitted = "stale-revision";
        return resolved;
    }
    if (!hasEvidence)
    {
        // 有输入但被安全门禁清空（路径当 ID、非法 ID）→ 明确记录，便于诊断且不含原文。
        resolved.omitted = (snapshot.sourceId != bounded.sourceId ||
                            snapshot.nodeId != bounded.nodeId)
                               ? "unsafe-id"
                               : "no-selection";
        return resolved;
    }

    resolved.present = true;
    resolved.text = bounded.text;
    resolved.truncated = bounded.truncated;
    resolved.dirty = bounded.dirty;
    resolved.nodeId = bounded.nodeId;
    resolved.sourceId = bounded.sourceId;  // Bound() 已保证 dirty 时为空
    resolved.startLine = bounded.startLine;
    resolved.endLine = bounded.endLine;
    return resolved;
}

}  // namespace agent::gui::selection
