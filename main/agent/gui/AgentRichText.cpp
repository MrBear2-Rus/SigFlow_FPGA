// AgentRichText.cpp — AGUI-T12 受限富文本解析实现
// （纯 C++20：无 wxWidgets、无 IO、无异常、无静态状态、无 locale、无正则、无文件系统/网络）
//
// 口径（与 AgentRichText.h 冻结契约一致，确定性可测）：
//   * 先统一换行：CRLF 与独立 CR 都变成 LF；除此之外不做任何转码，字节原样传递。
//   * 块识别顺序：围栏代码 → 管道表格 → 无序/有序列表 → 段落；空白行只作块分隔、本身被跳过。
//   * 行内只认 `**粗体**` 与 `` `行内代码` ``，且不嵌套；HTML/实体/URL/路径一律是普通文字。
//   * 不丢内容：未闭合的定界符、畸形表格、不认识的语法、超长文本都按文本保留，
//     不做静默截断；空输入产出空文档，非空输入至少产出一个块。

#include "agent/gui/AgentRichText.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace agent::gui::rich {
namespace {

// ---------------------------------------------------------------- 行级小工具

/// 只把空格/制表符当作"行内空白"；其余字节（含 UTF-8 多字节序列）都是文字。
bool IsBlankChar(char c)
{
    return c == ' ' || c == '\t';
}

bool IsDigitChar(char c)
{
    return c >= '0' && c <= '9';
}

/// 空白行 = 空串或只由空格/制表符组成。它们只作块分隔，不产生块。
bool IsBlankLine(const std::string& line)
{
    for (const char c : line)
    {
        if (!IsBlankChar(c))
        {
            return false;
        }
    }
    return true;
}

/// 首个非空白字符下标（没有则等于 line.size()）。
std::size_t FirstNonBlank(const std::string& line)
{
    std::size_t i = 0;
    while (i < line.size() && IsBlankChar(line[i]))
    {
        ++i;
    }
    return i;
}

/// 去掉左右空格/制表符（只去一层扫描，不是递归裁剪）。
std::string TrimBlanks(const std::string& line)
{
    std::size_t begin = 0;
    std::size_t end   = line.size();
    while (begin < end && IsBlankChar(line[begin]))
    {
        ++begin;
    }
    while (end > begin && IsBlankChar(line[end - 1]))
    {
        --end;
    }
    return line.substr(begin, end - begin);
}

std::size_t CountChar(const std::string& line, char wanted)
{
    std::size_t count = 0;
    for (const char c : line)
    {
        if (c == wanted)
        {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------- run 装配

/// 追加一段 run；与上一段格式相同就合并（保证相邻同格式 run 只有一段）。
void AppendRun(std::vector<Run>& runs, const std::string& text, bool bold, bool code)
{
    if (text.empty())
    {
        return;
    }
    if (!runs.empty() && runs.back().bold == bold && runs.back().code == code)
    {
        runs.back().text += text;
        return;
    }
    Run run;
    run.text = text;
    run.bold = bold;
    run.code = code;
    runs.push_back(std::move(run));
}

void AppendRuns(std::vector<Run>& target, const std::vector<Run>& source)
{
    for (const Run& run : source)
    {
        AppendRun(target, run.text, run.bold, run.code);
    }
}

std::string RunsText(const std::vector<Run>& runs)
{
    std::string text;
    for (const Run& run : runs)
    {
        text += run.text;
    }
    return text;
}

// ---------------------------------------------------------------- 行内解析（规则 6）

/// 段落 / 列表项 / 表格单元格的行内解析：
///   `**` 找严格更后的 `**`，内容非空才算粗体；单个反引号找更后的单反引号，内容非空才算行内代码。
///   配对失败时这两个字符按字面量留在普通文字里；粗体内容与代码内容都不再二次解析（无嵌套）。
std::vector<Run> ParseInline(const std::string& text)
{
    std::vector<Run> runs;
    std::string plain;
    const std::size_t size = text.size();
    std::size_t i          = 0;

    while (i < size)
    {
        const char c = text[i];

        if (c == '*' && i + 1 < size && text[i + 1] == '*')
        {
            const std::size_t close = text.find("**", i + 2);
            if (close != std::string::npos && close > i + 2)
            {
                if (!plain.empty())
                {
                    AppendRun(runs, plain, false, false);
                    plain.clear();
                }
                AppendRun(runs, text.substr(i + 2, close - i - 2), true, false);
                i = close + 2;
                continue;
            }
            // 未配对（或内容为空）：两个字符都是字面量。
            plain += c;
            plain += text[i + 1];
            i += 2;
            continue;
        }

        if (c == '`')
        {
            const std::size_t close = text.find('`', i + 1);
            if (close != std::string::npos && close > i + 1)
            {
                if (!plain.empty())
                {
                    AppendRun(runs, plain, false, false);
                    plain.clear();
                }
                AppendRun(runs, text.substr(i + 1, close - i - 1), false, true);
                i = close + 1;
                continue;
            }
            plain += c;
            ++i;
            continue;
        }

        plain += c;
        ++i;
    }

    if (!plain.empty())
    {
        AppendRun(runs, plain, false, false);
    }
    return runs;
}

// ---------------------------------------------------------------- 围栏代码（规则 3）

struct FenceOpen
{
    bool fence = false;
    std::string info;
};

/// info token 允许的字符：字母、数字、下划线、加号、点、减号（不含空格）。
bool IsInfoChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigitChar(c) || c == '_' ||
           c == '+' || c == '.' || c == '-';
}

/// 首非空白字符起是 >=3 个反引号，其余要么为空、要么是合法的 info token，才算围栏开始。
FenceOpen DetectFenceOpen(const std::string& line)
{
    FenceOpen open;
    const std::size_t start = FirstNonBlank(line);
    std::size_t ticks       = 0;
    while (start + ticks < line.size() && line[start + ticks] == '`')
    {
        ++ticks;
    }
    if (ticks < 3)
    {
        return open;
    }
    const std::size_t rest = start + ticks;
    for (std::size_t i = rest; i < line.size(); ++i)
    {
        if (!IsInfoChar(line[i]))
        {
            return open;  // 例如 ```cpp 之后还有空格/反引号：不是围栏，按普通文字处理
        }
    }
    open.fence = true;
    open.info  = line.substr(rest);
    return open;
}

/// 闭合围栏：trim 之后"恰好"是 >=3 个反引号（不带 info token）。
bool IsFenceClose(const std::string& line)
{
    const std::string trimmed = TrimBlanks(line);
    if (trimmed.size() < 3)
    {
        return false;
    }
    for (const char c : trimmed)
    {
        if (c != '`')
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- 列表（规则 5）

enum class ListKind
{
    None = 0,
    Bullet,
    Ordered
};

struct ListMarker
{
    ListKind kind = ListKind::None;
    std::string content;  ///< 标记之后（跳过空白）的剩余文字，未做行内解析
};

std::string ResidueAfter(const std::string& line, std::size_t from)
{
    std::size_t i = from;
    while (i < line.size() && IsBlankChar(line[i]))
    {
        ++i;
    }
    return line.substr(i);
}

/// `- ` / `* ` 为无序；`^\d{1,3}[.)]\s+` 为有序。判定基于 trim 后的形态，内容取自原始行。
ListMarker DetectListMarker(const std::string& line)
{
    ListMarker marker;
    const std::size_t size = line.size();
    const std::size_t i    = FirstNonBlank(line);
    if (i >= size)
    {
        return marker;
    }

    if (line[i] == '-' || line[i] == '*')
    {
        if (i + 1 < size && IsBlankChar(line[i + 1]))
        {
            marker.kind    = ListKind::Bullet;
            marker.content = ResidueAfter(line, i + 2);
        }
        return marker;
    }

    std::size_t digits = 0;
    std::size_t j      = i;
    while (j < size && IsDigitChar(line[j]) && digits < 4)
    {
        ++j;
        ++digits;
    }
    if (digits < 1 || digits > 3 || j >= size || (line[j] != '.' && line[j] != ')'))
    {
        return marker;
    }
    ++j;
    if (j >= size || !IsBlankChar(line[j]))
    {
        return marker;
    }
    marker.kind    = ListKind::Ordered;
    marker.content = ResidueAfter(line, j);
    return marker;
}

// ---------------------------------------------------------------- 管道表格（规则 4）

/// 按 '|' 切单元格：先去左右空白与外侧一条 `|`，再逐格 trim。
std::vector<std::string> SplitCells(const std::string& line)
{
    std::string body = TrimBlanks(line);
    if (!body.empty() && body.front() == '|')
    {
        body.erase(0, 1);
    }
    if (!body.empty() && body.back() == '|')
    {
        body.pop_back();
    }

    std::vector<std::string> cells;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t bar = body.find('|', start);
        if (bar == std::string::npos)
        {
            cells.push_back(TrimBlanks(body.substr(start)));
            break;
        }
        cells.push_back(TrimBlanks(body.substr(start, bar - start)));
        start = bar + 1;
    }
    return cells;
}

/// 分隔单元格：2 个及以上减号，两端可选对齐冒号；其余字符一律不接受。
bool IsSeparatorCell(const std::string& cell)
{
    const std::size_t size = cell.size();
    std::size_t i          = 0;
    if (i < size && cell[i] == ':')
    {
        ++i;
    }
    std::size_t dashes = 0;
    while (i < size && cell[i] == '-')
    {
        ++i;
        ++dashes;
    }
    if (dashes < 2)
    {
        return false;
    }
    if (i < size && cell[i] == ':')
    {
        ++i;
    }
    return i == size;
}

bool IsSeparatorRow(const std::string& line)
{
    const std::vector<std::string> cells = SplitCells(line);
    if (cells.empty())
    {
        return false;
    }
    for (const std::string& cell : cells)
    {
        if (!IsSeparatorCell(cell))
        {
            return false;
        }
    }
    return true;
}

/// 表格开始：本行至少两个 '|'，且下一行是合格的分隔行。没有分隔行就不是表格。
bool IsTableStart(const std::vector<std::string>& lines, std::size_t index)
{
    if (CountChar(lines[index], '|') < 2)
    {
        return false;
    }
    if (index + 1 >= lines.size())
    {
        return false;
    }
    return IsSeparatorRow(lines[index + 1]);
}

/// 表格数据行：非空行、非围栏、非列表项、且至少含一个 '|'。
bool IsTableBodyLine(const std::string& line)
{
    if (IsBlankLine(line))
    {
        return false;
    }
    if (DetectFenceOpen(line).fence)
    {
        return false;
    }
    if (DetectListMarker(line).kind != ListKind::None)
    {
        return false;
    }
    return line.find('|') != std::string::npos;
}

// ---------------------------------------------------------------- 文本切分

/// CRLF 与独立 CR 统一成 LF；不做其它转码。
std::string NormalizeNewlines(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r')
        {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n')
            {
                ++i;
            }
            continue;
        }
        out += text[i];
    }
    return out;
}

/// 按 LF 切行；末尾换行会留下一个空行走廊，空行在块循环里直接被跳过。
std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos)
        {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

// ---------------------------------------------------------------- 块解析

/// 围栏代码：逐行原样收集，直到闭合围栏或输入结束；未闭合就吃掉剩余全部输入。
std::size_t ParseFence(const std::vector<std::string>& lines, std::size_t index, Document& document)
{
    const FenceOpen open = DetectFenceOpen(lines[index]);

    std::string collected;
    bool hasCollected = false;
    std::size_t cursor = index + 1;
    while (cursor < lines.size() && !IsFenceClose(lines[cursor]))
    {
        if (hasCollected)
        {
            collected += '\n';
        }
        collected += lines[cursor];
        hasCollected = true;
        ++cursor;
    }

    Block block;
    block.kind  = BlockKind::CodeBlock;
    block.text  = collected;
    block.info  = open.info;
    document.blocks.push_back(std::move(block));

    if (cursor < lines.size())
    {
        ++cursor;  // 跳过闭合围栏（围栏本身不属于内容）
    }
    return cursor;
}

std::size_t ParseTable(const std::vector<std::string>& lines, std::size_t index, Document& document)
{
    Block block;
    block.kind      = BlockKind::Table;
    block.hasHeader = true;

    const std::vector<std::string> headerCells = SplitCells(lines[index]);
    const std::size_t width                    = headerCells.empty() ? 1 : headerCells.size();
    block.header.cells.reserve(width);
    for (const std::string& cell : headerCells)
    {
        TableCell parsed;
        parsed.runs = ParseInline(cell);
        block.header.cells.push_back(std::move(parsed));
    }

    std::size_t cursor = index + 2;  // 表头行 + 分隔行
    while (cursor < lines.size() && IsTableBodyLine(lines[cursor]))
    {
        const std::vector<std::string> cells = SplitCells(lines[cursor]);
        TableRow row;
        row.cells.reserve(width);
        for (std::size_t i = 0; i < width; ++i)
        {
            TableCell parsed;
            if (i < cells.size())
            {
                parsed.runs = ParseInline(cells[i]);
            }
            row.cells.push_back(std::move(parsed));  // 短行补空格子
        }
        // 超宽行：多出来的单元格文字并入最后一格，用 " | " 分隔，保证不丢失。
        for (std::size_t i = width; i < cells.size(); ++i)
        {
            AppendRun(row.cells[width - 1].runs, " | ", false, false);
            AppendRuns(row.cells[width - 1].runs, ParseInline(cells[i]));
        }
        block.rows.push_back(std::move(row));
        ++cursor;
    }

    document.blocks.push_back(std::move(block));
    return cursor;
}

/// 同一标记种类连续若干行合成一个列表块；标识种类变化/空行/围栏/表格之外的行都会收束该块。
std::size_t ParseList(const std::vector<std::string>& lines, std::size_t index, ListKind kind,
                      Document& document)
{
    Block block;
    block.kind = kind == ListKind::Ordered ? BlockKind::OrderedList : BlockKind::BulletList;

    std::size_t cursor = index;
    while (cursor < lines.size())
    {
        const ListMarker marker = DetectListMarker(lines[cursor]);
        if (marker.kind != kind)
        {
            break;
        }
        ListItem item;
        item.runs = ParseInline(marker.content);
        block.items.push_back(std::move(item));
        ++cursor;
    }

    document.blocks.push_back(std::move(block));
    return cursor;
}

/// 段落：连续的非空/非围栏/非表格头/非列表行，用 '\n' 连接后整体做行内解析（保留换行）。
std::size_t ParseParagraph(const std::vector<std::string>& lines, std::size_t index,
                           Document& document)
{
    std::string joined;
    bool hasLine       = false;
    std::size_t cursor = index;
    while (cursor < lines.size())
    {
        if (IsBlankLine(lines[cursor]))
        {
            break;
        }
        if (DetectFenceOpen(lines[cursor]).fence)
        {
            break;
        }
        if (IsTableStart(lines, cursor))
        {
            break;
        }
        if (DetectListMarker(lines[cursor]).kind != ListKind::None)
        {
            break;
        }
        if (hasLine)
        {
            joined += '\n';
        }
        joined += lines[cursor];
        hasLine = true;
        ++cursor;
    }

    Block block;
    block.kind = BlockKind::Paragraph;
    block.runs = ParseInline(joined);
    document.blocks.push_back(std::move(block));
    return cursor;
}

// ---------------------------------------------------------------- 拍平

std::string RowPlainText(const TableRow& row)
{
    std::string text;
    for (std::size_t i = 0; i < row.cells.size(); ++i)
    {
        if (i != 0)
        {
            text += " | ";
        }
        text += RunsText(row.cells[i].runs);
    }
    return text;
}

/// 单块的可读文本；块之间由 PlainText 用空行连接。
std::string BlockPlainText(const Block& block)
{
    switch (block.kind)
    {
        case BlockKind::Paragraph:
            return RunsText(block.runs);

        case BlockKind::BulletList:
        case BlockKind::OrderedList:
        {
            std::string text;
            for (std::size_t i = 0; i < block.items.size(); ++i)
            {
                if (i != 0)
                {
                    text += '\n';
                }
                if (block.kind == BlockKind::BulletList)
                {
                    text += "- ";
                }
                else
                {
                    text += std::to_string(i + 1);
                    text += ". ";
                }
                text += RunsText(block.items[i].runs);
            }
            return text;
        }

        case BlockKind::CodeBlock:
            return block.text;

        case BlockKind::Table:
        {
            std::string text;
            bool hasLine = false;
            if (!block.header.cells.empty())
            {
                text += RowPlainText(block.header);
                hasLine = true;
            }
            for (const TableRow& row : block.rows)
            {
                if (hasLine)
                {
                    text += '\n';
                }
                text += RowPlainText(row);
                hasLine = true;
            }
            return text;
        }

        default:
            return std::string();
    }
}

}  // namespace

// ---------------------------------------------------------------- 公开接口

Document Parse(const std::string& utf8)
{
    Document document;
    const std::vector<std::string> lines = SplitLines(NormalizeNewlines(utf8));

    std::size_t cursor = 0;
    while (cursor < lines.size())
    {
        if (IsBlankLine(lines[cursor]))
        {
            ++cursor;  // 空行只分隔块
            continue;
        }
        if (DetectFenceOpen(lines[cursor]).fence)
        {
            cursor = ParseFence(lines, cursor, document);
            continue;
        }
        if (IsTableStart(lines, cursor))
        {
            cursor = ParseTable(lines, cursor, document);
            continue;
        }
        const ListMarker marker = DetectListMarker(lines[cursor]);
        if (marker.kind != ListKind::None)
        {
            cursor = ParseList(lines, cursor, marker.kind, document);
            continue;
        }
        cursor = ParseParagraph(lines, cursor, document);
    }

    return document;
}

std::string PlainText(const Document& document)
{
    std::string out;
    bool first = true;
    for (const Block& block : document.blocks)
    {
        const std::string chunk = BlockPlainText(block);
        if (chunk.empty())
        {
            continue;
        }
        if (!first)
        {
            out += "\n\n";
        }
        out += chunk;
        first = false;
    }
    return out;
}

std::string AllRunText(const Document& document)
{
    std::string out;
    for (const Block& block : document.blocks)
    {
        switch (block.kind)
        {
            case BlockKind::Paragraph:
                out += RunsText(block.runs);
                break;

            case BlockKind::BulletList:
            case BlockKind::OrderedList:
                for (const ListItem& item : block.items)
                {
                    out += RunsText(item.runs);
                }
                break;

            case BlockKind::CodeBlock:
                out += block.text;
                break;

            case BlockKind::Table:
                for (const TableCell& cell : block.header.cells)
                {
                    out += RunsText(cell.runs);
                }
                for (const TableRow& row : block.rows)
                {
                    for (const TableCell& cell : row.cells)
                    {
                        out += RunsText(cell.runs);
                    }
                }
                break;

            default:
                break;
        }
    }
    return out;
}

}  // namespace agent::gui::rich
