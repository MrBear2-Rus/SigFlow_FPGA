#pragma once
#include <wx/stc/stc.h>
#include <string>

class TerminalController;   // 后台控制器
class wxWindow;

class TerminalCtrl : public wxStyledTextCtrl
    //继承wxStyledTextCtrl
{
public:
    TerminalCtrl(wxWindow* parent);

    void PrintOutput(const wxString& text);
    void PrintError(const wxString& text);
    void BeginProcessOutput(const wxString& header);
    void AppendProcessOutput(const wxString& text);
    void FinishProcessOutput(const wxString& summary);

    /// T-660（SPEC PLT-009）：DPI / 主题变化后重设字体与语义样式。
    /// 转发私有的 ApplyTerminalStyle()，**只改外观**（背景/字体/行距/样式号），
    /// 不改写任何输出文本内容（SAFE-009 / SAFE-010 / TEST-022 依赖文本逐字节不变）。
    void ReapplyStyle() { ApplyTerminalStyle(); }

private:
    TerminalController* m_controller;
    int m_promptPos;  // 当前提示符起始位置

    void OnChar(wxKeyEvent& event);
    void SubmitCurrentLine();

    // ---- T-320 终端外观（SPEC CMP-062 / SAFE-012）----
    // 说明：以下辅助函数**只改外观**（背景 / 字体 / 行距 / 样式号），
    // 不参与、也不改写任何输出文本内容（SAFE-009 / SAFE-010 / TEST-022 依赖文本逐字节不变）。
    void ApplyTerminalStyle();                             // termBg + 等宽 P12 + 行距 1.7 + 语义样式表
    void SetRangeStyle(int start, int end, int styleId);   // 对已插入区间上样式号（字节偏移口径）
    void AppendPrompt();                                   // 追加 "> "（字节不变）并着提示符色
    void AppendClassifiedOutput(const wxString& text);     // 按行前缀分类着色的追加
    void ApplyLineStyle(int start, const wxString& line);  // 单行分类 → 样式区间

    wxDECLARE_EVENT_TABLE();
};
