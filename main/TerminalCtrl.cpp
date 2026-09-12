#include "TerminalCtrl.h"//引用头文件
#include "TerminalController.h" 
//事件表
wxBEGIN_EVENT_TABLE(TerminalCtrl, wxStyledTextCtrl)
   EVT_KEY_DOWN(TerminalCtrl::OnChar)
wxEND_EVENT_TABLE()

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

    // 设置字体
    wxFont font(10, wxFONTFAMILY_MODERN,
        wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL);
    StyleSetFont(wxSTC_STYLE_DEFAULT, font);
    StyleClearAll();

    // 初始提示符
    AppendText("> ");
    m_promptPos = GetCurrentPos();
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

    AppendText("\n");

    std::string output, error;
    m_controller->submitLine(
        line.ToStdString(), output, error
    );

    if (!output.empty())
        PrintOutput(output);

    if (!error.empty())
        PrintError(error);

    AppendText("> ");

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
    AppendText(text);
    if (!text.EndsWith("\n"))
        AppendText("\n> ");
}

void TerminalCtrl::PrintError(const wxString& text)
{
    // 简单做法：加前缀
    AppendText("[ERROR] " + text + "\n");
}

void TerminalCtrl::BeginProcessOutput(const wxString& header)
{
    if (!GetText().EndsWith("\n")) {
        AppendText("\n");
    }
    AppendText(header + "\n");
    m_promptPos = GetTextLength();
    SetCurrentPos(m_promptPos);
    SetSelection(m_promptPos, m_promptPos);
}

void TerminalCtrl::AppendProcessOutput(const wxString& text)
{
    if (text.IsEmpty()) {
        return;
    }

    AppendText(text);
    m_promptPos = GetTextLength();
    SetCurrentPos(m_promptPos);
    SetSelection(m_promptPos, m_promptPos);
}

void TerminalCtrl::FinishProcessOutput(const wxString& summary)
{
    if (!GetText().EndsWith("\n")) {
        AppendText("\n");
    }
    AppendText(summary + "\n> ");
    m_promptPos = GetTextLength();
    SetCurrentPos(m_promptPos);
    SetSelection(m_promptPos, m_promptPos);
}
