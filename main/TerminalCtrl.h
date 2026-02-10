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

private:
    TerminalController* m_controller;
    int m_promptPos;  // 当前提示符起始位置

    void OnChar(wxKeyEvent& event);
    void SubmitCurrentLine();
    wxDECLARE_EVENT_TABLE();
};
