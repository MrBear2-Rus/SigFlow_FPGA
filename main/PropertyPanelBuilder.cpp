#include "PropertyPanelBuilder.h"

// 辅助函数：设置只读文本框的背景色与父窗口一致
static void SetReadOnlyBackground(wxTextCtrl* ctrl, wxWindow* parent) {
    ctrl->SetEditable(false);
    ctrl->SetBackgroundColour(parent->GetBackgroundColour());
}

wxSizer* PropertyPanelBuilder::CreateSectionTitle(wxWindow* parent, const wxString& title) {
    auto vSizer = new wxBoxSizer(wxVERTICAL);
    auto label = new wxStaticText(parent, wxID_ANY, title);
    wxFont font = label->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    label->SetFont(font);
    label->SetForegroundColour(wxColour(0, 102, 204)); // 深蓝色
    vSizer->Add(label, 0, wxTOP | wxLEFT | wxRIGHT, 10);
    vSizer->Add(new wxStaticLine(parent, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
    return vSizer;
}

wxSizer* PropertyPanelBuilder::CreateTextRow(wxWindow* parent, const wxString& label, const wxString& value) {
    auto hSizer = new wxBoxSizer(wxHORIZONTAL);
    hSizer->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto txt = new wxTextCtrl(parent, wxID_ANY, value);
    SetReadOnlyBackground(txt, parent);
    hSizer->Add(txt, 1, wxEXPAND);
    return hSizer;
}

wxSizer* PropertyPanelBuilder::CreateIdentifierRow(wxWindow* parent, const wxString& label,
    const wxString& initialValue, wxTextCtrl** textCtrlOut) {
    auto hSizer = new wxBoxSizer(wxHORIZONTAL);
    hSizer->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto txt = new wxTextCtrl(parent, wxID_ANY, initialValue,
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    txt->SetEditable(true);
    hSizer->Add(txt, 1, wxEXPAND);
    if (textCtrlOut) *textCtrlOut = txt;
    return hSizer;
}

wxSizer* PropertyPanelBuilder::CreateEditableTextRow(wxWindow* parent, const wxString& label,
    const wxString& initialValue, wxTextCtrl** textCtrlOut) {
    auto hSizer = new wxBoxSizer(wxHORIZONTAL);
    hSizer->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    auto txt = new wxTextCtrl(parent, wxID_ANY, initialValue,
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    txt->SetEditable(true);
    hSizer->Add(txt, 1, wxEXPAND);
    if (textCtrlOut) *textCtrlOut = txt;
    return hSizer;
}

wxSizer* PropertyPanelBuilder::CreateChoiceRow(wxWindow* parent, const wxString& label,
    const wxArrayString& choices, int initialSelection,
    wxChoice** choiceOut) {
    auto hSizer = new wxBoxSizer(wxHORIZONTAL);
    hSizer->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    auto ch = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    ch->SetSelection(initialSelection);
    hSizer->Add(ch, 1, wxEXPAND);
    if (choiceOut) *choiceOut = ch;
    return hSizer;
}

wxSizer* PropertyPanelBuilder::CreateTopPortRow(wxWindow* parent, const wxString& direction,
    const wxString& portName,
    wxTextCtrl** nameCtrlOut, wxButton** deleteBtnOut) {
    auto rowSizer = new wxBoxSizer(wxHORIZONTAL);
    auto dirCtrl = new wxTextCtrl(parent, wxID_ANY, direction);
    SetReadOnlyBackground(dirCtrl, parent);
    rowSizer->Add(dirCtrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

    auto nameCtrl = new wxTextCtrl(parent, wxID_ANY, portName,
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    nameCtrl->SetEditable(true);
    rowSizer->Add(nameCtrl, 1, wxEXPAND | wxLEFT, 10);
    if (nameCtrlOut) *nameCtrlOut = nameCtrl;

    auto delBtn = new wxButton(parent, wxID_ANY, "x", wxDefaultPosition, wxSize(25, 25));
    delBtn->SetToolTip("Delete this port");
    rowSizer->Add(delBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);
    if (deleteBtnOut) *deleteBtnOut = delBtn;

    // 整个行包装在垂直 sizer 中，以便添加分割线（由调用者添加）
    auto wrapper = new wxBoxSizer(wxVERTICAL);
    wrapper->Add(rowSizer, 0, wxEXPAND);
    wrapper->Add(new wxStaticLine(parent, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
    return wrapper;
}

wxSizer* PropertyPanelBuilder::CreateSecondPortRow(wxWindow* parent, const wxString& direction,
    const wxString& portName, const wxString& connValue,
    const wxArrayString& choices, // 新增：信号选项列表
    wxChoice** connChoiceOut,     // 修改：返回 Choice 而不是 TextCtrl
    wxButton** deleteBtnOut)
{
    auto rowSizer = new wxBoxSizer(wxHORIZONTAL);
    auto dirCtrl = new wxTextCtrl(parent, wxID_ANY, direction);
    SetReadOnlyBackground(dirCtrl, parent);
    rowSizer->Add(dirCtrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

    auto nameCtrl = new wxTextCtrl(parent, wxID_ANY, portName);
    SetReadOnlyBackground(nameCtrl, parent);
    rowSizer->Add(nameCtrl, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);

    auto colon = new wxStaticText(parent, wxID_ANY, "connected by");
    colon->SetForegroundColour(wxColour(120, 120, 120));
    rowSizer->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);

    auto connChoice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    connChoice->SetBackgroundColour(wxColour(240, 248, 255));

    int selection = connChoice->FindString(connValue);
    if (selection != wxNOT_FOUND) {
        connChoice->SetSelection(selection);
    }

    rowSizer->Add(connChoice, 1, wxEXPAND | wxRIGHT, 5);
    if (connChoice) *connChoiceOut = connChoice;

    // 删除按钮
    if (deleteBtnOut) {
        auto delBtn = new wxButton(parent, wxID_ANY, "X", wxDefaultPosition, wxSize(25, 25));
        delBtn->SetToolTip("Delete this port");
        rowSizer->Add(delBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        *deleteBtnOut = delBtn;
    }

    auto wrapper = new wxBoxSizer(wxVERTICAL);
    wrapper->Add(rowSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 5);
    wrapper->Add(new wxStaticLine(parent, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
    return wrapper;
}

wxSizer* PropertyPanelBuilder::CreateNBOrBExpressionRow(wxWindow* parent, float delay,
    const wxString& outPortName,
    bool isBlocking, const wxString& rhs,
    wxTextCtrl** delayCtrlOut,
    wxChoice** opChoiceOut,
    wxTextCtrl** rhsCtrlOut,
    wxButton** deleteBtnOut)
{
    auto rowSizer = new wxBoxSizer(wxHORIZONTAL);

    // 添加 "Expression:" 标签
    auto exprLabel = new wxStaticText(parent, wxID_ANY, "Expression:");
    rowSizer->Add(exprLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // # 和 delay 文本框
    auto hash = new wxStaticText(parent, wxID_ANY, "#");
    rowSizer->Add(hash, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
    auto delayCtrl = new wxTextCtrl(parent, wxID_ANY, wxString::Format("%.1f", delay),
        wxDefaultPosition, wxSize(40, -1), wxTE_PROCESS_ENTER);
    rowSizer->Add(delayCtrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    if (delayCtrlOut) *delayCtrlOut = delayCtrl;

    // 输出端口名（只读）
    auto outCtrl = new wxTextCtrl(parent, wxID_ANY, outPortName, wxDefaultPosition, wxSize(60, -1));
    SetReadOnlyBackground(outCtrl, parent);
    rowSizer->Add(outCtrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // 操作符选择
    wxArrayString choices;
    choices.Add("=");   // blocking
    choices.Add("<=");  // non-blocking
    auto opChoice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    opChoice->SetSelection(isBlocking ? 0 : 1);
    rowSizer->Add(opChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    if (opChoiceOut) *opChoiceOut = opChoice;

    // RHS 文本框
    auto rhsCtrl = new wxTextCtrl(parent, wxID_ANY, rhs,
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    rhsCtrl->SetHint("e.g. In1 & In2");
    rhsCtrl->SetMinSize(wxSize(300, -1));
    rowSizer->Add(rhsCtrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    if (rhsCtrlOut) *rhsCtrlOut = rhsCtrl;

    // 删除按钮
    if (deleteBtnOut) {
        auto delBtn = new wxButton(parent, wxID_ANY, "X", wxDefaultPosition, wxSize(25, 25));
        delBtn->SetToolTip("Delete this expression");
        rowSizer->Add(delBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        *deleteBtnOut = delBtn;
    }

    // 包装成垂直 sizer（如果需要分割线，由调用者添加）
    auto wrapper = new wxBoxSizer(wxVERTICAL);
    wrapper->Add(rowSizer, 0, wxEXPAND);
    return wrapper;
}

wxButton* PropertyPanelBuilder::CreateButton(wxWindow* parent, const wxString& label,
    const wxColour& foreground) {
    auto btn = new wxButton(parent, wxID_ANY, label);
    if (foreground.IsOk())
        btn->SetForegroundColour(foreground);
    return btn;
}
