#include "TerminalCtrl.h"//引用头文件
#include "TerminalController.h" 

#include "ui/UiTheme.h"   // T-320：唯一取色/字体来源（SAFE-012 / THE-001）

//事件表
wxBEGIN_EVENT_TABLE(TerminalCtrl, wxStyledTextCtrl)
   EVT_KEY_DOWN(TerminalCtrl::OnChar)
wxEND_EVENT_TABLE()

namespace {

// ---------------------------------------------------------------------------
// T-320 终端语义样式号（SPEC CMP-062）
// 40 起为自定义区间：wxSTC 预定义样式占 32..39（wxSTC_STYLE_LASTPREDEFINED = 39，
// wxSTC_STYLE_MAX = 255）；本控件不设 lexer，40..45 不会被任何词法器复用。
// ---------------------------------------------------------------------------
const int kTermStyleOutput  = wxSTC_STYLE_LASTPREDEFINED + 1;  // 40 普通输出   → 深色档 text2
const int kTermStylePrompt  = kTermStyleOutput + 1;            // 41 提示符 "> " → 深色档 text3
const int kTermStyleCommand = kTermStyleOutput + 2;            // 42 命令行     → 深色档 accent
const int kTermStyleReady   = kTermStyleOutput + 3;            // 43 "[Ready]"  → 深色档 brand
const int kTermStyleWarn    = kTermStyleOutput + 4;            // 44 "[WARN]"   → 深色档 warn
const int kTermStyleError   = kTermStyleOutput + 5;            // 45 "[ERROR]"  → 深色档 danger
const int kTermStyleCount   = 6;

// SPEC §3.5：termBg 刻意不随浅色档变化（浅深两档同值）。
// 因此终端内文字同样固定取**深色档** token，保证浅色档下仍是深底浅字（可读性）：
// 提示符/命令行/语义行分别取深色档 text3 / accent / brand / warn / danger。
const UiTheme::Colours& TermColours()
{
    return UiTheme::TerminalPalette();
}

// SPEC CMP-062 行距 1.7。
// Scintilla 没有直接的行距 API，行高 = 字体 ascent + descent + extraAscent + extraDescent，
// 故在字体度量上下各补 (行距倍数 - 1.0) / 2 的额外像素（12 DIP 字号 → 上下各 4 DIP，
// 行高约 20 DIP ≈ 1.67 倍，最接近 1.7 的整数 DIP 近似）。
const int kTermLineSpacingPercent = 170;

int TermExtraHalfDip()
{
    const int baseDip  = static_cast<int>(UiTheme::TextSize::P12);            // 12 DIP
    const int extraDip = baseDip * kTermLineSpacingPercent / 100 - baseDip;   // ≈ 8 DIP
    return extraDip / 2;                                                      // 4 DIP
}

}  // namespace

//构造函数，初始化为终端模式
TerminalCtrl::TerminalCtrl(wxWindow* parent)
    : wxStyledTextCtrl(parent, wxID_ANY)
{

    m_controller = new TerminalController();
    // 基本配置
    SetWrapMode(wxSTC_WRAP_WORD);
    SetUseHorizontalScrollBar(false);

    // 不显示 margin 行号
    SetMarginWidth(0, 0);

    // T-320：字体 / 深底 / 行距 / 语义样式表（全部取自 UiTheme，无颜色字面量）
    ApplyTerminalStyle();

    // 初始提示符
    AppendPrompt();
    m_promptPos = GetCurrentPos();
}

// T-320：终端外观（SPEC CMP-062）。只设置外观，不触碰任何文本内容。
void TerminalCtrl::ApplyTerminalStyle()
{
    const UiTheme::Colours& term = TermColours();
    const wxColour termBg = UiTheme::Palette().termBg;                    // SPEC §3.5 补充 token
    const wxFont   mono   = UiTheme::MonoFont(this, UiTheme::TextSize::P12);  // 12 DIP 等宽（PLT-002）

    // 默认样式：既是 STYLE_DEFAULT 模板，也是未显式着色文本（Scintilla 对新插入字节
    // 使用样式 0）的兜底前景 —— 用户键入过程中的命令文本即为深色档 text1。
    StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
    StyleSetBackground(wxSTC_STYLE_DEFAULT, termBg);
    StyleSetForeground(wxSTC_STYLE_DEFAULT, term.text1);
    StyleClearAll();   // 把默认样式复制到全部样式号（含下面要用的 40..45）

    // 语义样式号：字体 + 底色
    const int styles[kTermStyleCount] = {
        kTermStyleOutput, kTermStylePrompt, kTermStyleCommand,
        kTermStyleReady,  kTermStyleWarn,   kTermStyleError
    };
    for (int i = 0; i < kTermStyleCount; ++i) {
        StyleSetFont(styles[i], mono);
        StyleSetBackground(styles[i], termBg);
    }

    // 语义样式号：前景（SPEC CMP-062 前缀 → token）
    StyleSetForeground(kTermStyleOutput,  term.text2);
    StyleSetForeground(kTermStylePrompt,  term.text3);
    StyleSetForeground(kTermStyleCommand, term.accent);
    StyleSetForeground(kTermStyleReady,   term.brand);
    StyleSetForeground(kTermStyleWarn,    term.warn);
    StyleSetForeground(kTermStyleError,   term.danger);

    // 控件自身底色（文本区之外的空白）与光标色
    SetBackgroundColour(termBg);
    SetCaretForeground(term.text1);
    // 终端选择态也固定使用终端语义色；避免切换主题/重绘后出现原生浅蓝
    // 选择框压在深底上的低对比度组合。文本与内容本身不被改写。
    SetSelBackground(true, term.accent);
    SetSelForeground(true, term.text1);
    SetWhitespaceForeground(false, term.text3);

    // 行距 1.7（SPEC CMP-062）
    const int extraHalfDip = TermExtraHalfDip();
    SetExtraAscent(FromDIP(extraHalfDip));
    SetExtraDescent(FromDIP(extraHalfDip));
}

// 对已插入区间 [start, end) 上样式号（字节偏移，与 GetTextLength/GetCurrentPos 同口径）
void TerminalCtrl::SetRangeStyle(int start, int end, int styleId)
{
    if (end <= start || start < 0)
        return;
    StartStyling(start);
    SetStyling(end - start, styleId);
}

// 追加提示符 "> "（与改造前字节完全一致），并着提示符色
void TerminalCtrl::AppendPrompt()
{
    const int start = GetTextLength();
    AppendText("> ");
    SetRangeStyle(start, GetTextLength(), kTermStylePrompt);
}

// 追加输出文本并按**行前缀**分类着色。
// 文本逐字节原样追加（不改写、不裁剪、不重排），仅对刚插入的区间上样式号：
// 全文档不做事后重扫，故不会影响 SAFE-009 / SAFE-010 / TEST-022 依赖的任何行文本。
void TerminalCtrl::AppendClassifiedOutput(const wxString& text)
{
    if (text.IsEmpty())
        return;

    const size_t total = text.length();
    size_t pos = 0;
    while (pos < total) {
        // 注意：wxString::find 返回 size_t，未命中为 wxString::npos（不是 wxNOT_FOUND）
        const size_t nl   = text.find('\n', pos);
        const bool   last = (nl == wxString::npos);
        const size_t stop = last ? total : nl + 1;
        const wxString line = text.Mid(pos, stop - pos);   // 含行尾 '\n'（若有）

        // wxSTC 的 AppendText 走 SCI_APPENDTEXT：始终追加到文档末尾且不改动光标，
        // 因此 Append 前的 GetTextLength() 就是本次插入区间的起始字节偏移。
        const int start = GetTextLength();
        AppendText(line);
        ApplyLineStyle(start, line);

        if (last)
            break;
        pos = stop;
    }
}

// 单行分类：只看行首前缀（匹配失败 = 普通输出）。start 为该行的起始字节偏移。
void TerminalCtrl::ApplyLineStyle(int start, const wxString& line)
{
    const int end = GetTextLength();
    if (end <= start)
        return;

    // 允许行首空白：插件诊断行为 "  [<Status>] <id> <- <path>"（PluginHost.cpp:344-348）
    size_t indent = 0;
    while (indent < line.length() &&
           (line[indent] == ' ' || line[indent] == '\t')) {
        ++indent;
    }
    const wxString body = line.Mid(indent);

    if (body.StartsWith("[ERROR]") || body.StartsWith("[Error]")) {
        // PrintError 的 "[ERROR] " 前缀，以及插件诊断的 "[Error]" 状态行
        SetRangeStyle(start, end, kTermStyleError);
        return;
    }
    if (body.StartsWith("[WARN]")) {
        SetRangeStyle(start, end, kTermStyleWarn);
        return;
    }
    if (body.StartsWith("[Ready]")) {
        // SPEC CMP-062：整行普通色，仅首段 "[Ready]" 用 brand（SAFE-009 判据文本一字不动）
        SetRangeStyle(start, end, kTermStyleOutput);
        const int leadBytes  = static_cast<int>(line.Left(indent).ToUTF8().length());
        const int tokenBytes = static_cast<int>(wxString("[Ready]").ToUTF8().length());
        SetRangeStyle(start + leadBytes, start + leadBytes + tokenBytes, kTermStyleReady);
        return;
    }
    if (line.StartsWith("> ")) {
        SetRangeStyle(start, end, kTermStylePrompt);   // 以提示符开头的行
        return;
    }

    // 普通输出 → 深色档 text2
    SetRangeStyle(start, end, kTermStyleOutput);
}

//核心：键盘事件处理(接收所有字符）
void TerminalCtrl::OnChar(wxKeyEvent& event)
{
    int key = event.GetKeyCode();
    // 禁止在 prompt 之前修改
    if ((key == WXK_BACK || key == WXK_LEFT) &&
        GetCurrentPos() <= m_promptPos) {
        return;
    }

    // 回车 = 提交命令
    if (key == WXK_RETURN) {
        SubmitCurrentLine();
        return;
    }

    // 其余字符正常处理（包括复制、粘贴、输入）
    event.Skip();
    //skip()是关键，保证所有字符都能输入，ctrl+c/ctrl+v正常
}

//提交当前行命令（重点）
void TerminalCtrl::SubmitCurrentLine()
{
    int endPos = GetCurrentPos();

    // 向前找最近一次换行符
    int lineStart = endPos;
    while (lineStart > 0)
    {
        if (GetCharAt(lineStart - 1) == '\n')
            break;
        --lineStart;
    }

    // 去掉提示符 "> "
    if (GetTextRange(lineStart, lineStart + 2) == "> ")
        lineStart += 2;

    wxString line = GetTextRange(lineStart, endPos);

    // T-320 CMP-062：命令行回显着 accent —— 仅外观，命令文本本身不做任何改动
    SetRangeStyle(lineStart, endPos, kTermStyleCommand);

    AppendClassifiedOutput("\n");

    std::string output, error;
    m_controller->submitLine(
        line.ToStdString(), output, error
    );

    if (!output.empty())
        PrintOutput(output);

    if (!error.empty())
        PrintError(error);

    AppendPrompt();

    // ===== 新增：显式移动光标 =====
    int pos = GetTextLength();
    SetCurrentPos(pos);
    SetSelection(pos, pos);
    // ============================
    m_promptPos = GetCurrentPos();
}


//输出 & 错误打印
void TerminalCtrl::PrintOutput(const wxString& text)
{
    AppendClassifiedOutput(text);
    if (!text.EndsWith("\n")) {
        AppendClassifiedOutput("\n");
        AppendPrompt();
    }
}

void TerminalCtrl::PrintError(const wxString& text)
{
    // 简单做法：加前缀（文本一字不改；"[ERROR]" 由分类器着 danger 色）
    AppendClassifiedOutput("[ERROR] " + text + "\n");
}

void TerminalCtrl::BeginProcessOutput(const wxString& header)
{
    if (!GetText().EndsWith("\n")) {
        AppendClassifiedOutput("\n");
    }
    AppendClassifiedOutput(header + "\n");
    m_promptPos = GetTextLength();
    SetCurrentPos(m_promptPos);
    SetSelection(m_promptPos, m_promptPos);
}

void TerminalCtrl::AppendProcessOutput(const wxString& text)
{
    if (text.IsEmpty()) {
        return;
    }

    AppendClassifiedOutput(text);
    m_promptPos = GetTextLength();
    SetCurrentPos(m_promptPos);
    SetSelection(m_promptPos, m_promptPos);
}

void TerminalCtrl::FinishProcessOutput(const wxString& summary)
{
    if (!GetText().EndsWith("\n")) {
        AppendClassifiedOutput("\n");
    }
    AppendClassifiedOutput(summary + "\n");
    AppendPrompt();
    m_promptPos = GetTextLength();
    SetCurrentPos(m_promptPos);
    SetSelection(m_promptPos, m_promptPos);
}
