#pragma once
//
// AgentRichText — AGUI-T12 受限富文本解析（纯 C++20，无 wxWidgets 依赖）
//
// 设计源：GUI/agent-gui/index.html H191–265（.resp/.msg 排版：粗体、列表、表格、代码）、
//         spec.md §6.1/§12.2；design-review VIS-006/CMP-018；teaching-card.schema.json（body 是纯字符串）。
//
// 安全边界（必须遵守，调用方据此渲染）：
//   * 支持的是**受限 Markdown 子集**：`**粗体**`、`` `行内代码` ``、`- / *` 无序列表、
//     `1.` 有序列表、```` ``` ```` 围栏代码块、GFM 风格管道表格。除此之外一律是纯文本。
//   * **不解释 HTML**：任何 `<tag>`、`<script>`、实体、URL、路径都是普通文字（原样保留）。
//     这样恶意标记不会变成结构，也不会触发加载/脚本/链接。
//   * **不丢内容**：未闭合的标记、未知语法、超长文本都按文本保留，不做静默截断/丢弃。
//   * 输出只有文本 run 与块结构，没有链接、图片、执行对象；渲染层不调用系统/网络。
//
// 解析口径（可测试、确定性）：
//   CRLF→LF；块之间以空行分隔。识别顺序：围栏代码 → 管道表格 → 无序/有序列表 → 段落。
//   行内解析只在段落/列表项/表格单元格内进行；代码块内容逐字节保留（仅统一换行）。
//   粗体/行内代码不支持嵌套：先出现的定界符优先，未配对保持字面量。
#include <string>
#include <vector>

namespace agent::gui::rich {

enum class BlockKind
{
    Paragraph = 0,
    BulletList,
    OrderedList,
    CodeBlock,
    Table
};

/// 一段同格式的文本。渲染层只做字体（粗体/等宽）切换，不做任何标记解释。
struct Run
{
    std::string text;
    bool bold = false;
    bool code = false;
    bool operator==(const Run&) const = default;
};

struct ListItem
{
    std::vector<Run> runs;
    bool operator==(const ListItem&) const = default;
};

struct TableCell
{
    std::vector<Run> runs;
    bool operator==(const TableCell&) const = default;
};

struct TableRow
{
    std::vector<TableCell> cells;
    bool operator==(const TableRow&) const = default;
};

struct Block
{
    BlockKind kind = BlockKind::Paragraph;
    std::vector<Run> runs;        ///< Paragraph
    std::vector<ListItem> items;  ///< BulletList / OrderedList
    std::string text;             ///< CodeBlock 原始文本（不含围栏）
    std::string info;             ///< CodeBlock 语言标记（仅第一段 token，可能为空）
    TableRow header;              ///< Table 表头（无表头时 cells 为空）
    std::vector<TableRow> rows;   ///< Table 数据行
    bool hasHeader = false;
    bool operator==(const Block&) const = default;
};

struct Document
{
    std::vector<Block> blocks;
    bool operator==(const Document&) const = default;
};

/// 解析 UTF-8 文本。空输入返回空文档；任何输入都不会抛异常。
Document Parse(const std::string& utf8);

/// 把文档拍平成纯文本（保留所有文字，用于 tooltip / 测试 / 复制回退）。
std::string PlainText(const Document& document);

/// 所有 run 的文本按块顺序拼接（不插入额外分隔，便于逐字核对"没有丢内容"）。
std::string AllRunText(const Document& document);

}  // namespace agent::gui::rich
