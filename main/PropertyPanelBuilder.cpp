#include "PropertyPanelBuilder.h"

#include "ui/UiTheme.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>
#include <memory>

namespace {

// RV-T06：原生输入仍是实际交互控件，UiFieldChrome 只负责圆角壳、焦点/悬停
// 状态和安全内缩，不替换控件实例，也不改变 PropertyPanel 的业务绑定。
class UiFieldChrome final : public wxPanel
{
public:
    explicit UiFieldChrome(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(UiTheme::Palette().sunkenOnPanel);
        Bind(wxEVT_PAINT, &UiFieldChrome::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            LayoutContent();
            event.Skip();
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            m_hover = true;
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hover = false;
            Refresh(false);
            event.Skip();
        });
        m_themeSubscription = UiTheme::Subscribe([this] {
            Refresh(false);
            if (m_content != nullptr) {
                const auto& colours = UiTheme::Palette();
                m_content->SetBackgroundColour(colours.surfaceOnWindow);
                m_content->SetForegroundColour(colours.text1);
                m_content->Refresh(false);
            }
        });
    }

    void SetContent(wxWindow* content, bool readOnly)
    {
        m_content = content;
        m_readOnly = readOnly;
        if (m_content == nullptr)
            return;

        m_content->SetWindowStyleFlag(m_content->GetWindowStyleFlag() & ~wxBORDER_MASK);
        // 图三：输入框底色**统一**，只读/可编辑同色（原来的 bgPanel/sunken 色差会在
        // 壳内形成一圈"边框"观感，用户实测：仍有边框、颜色不一致）。
        m_content->SetBackgroundColour(UiTheme::Palette().surfaceOnWindow);
        m_content->SetForegroundColour(UiTheme::Palette().text1);   // 三个框字体颜色统一
        m_content->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) {
            m_focus = true;
            Refresh(false);
            event.Skip();
        });
        m_content->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
            m_focus = false;
            Refresh(false);
            event.Skip();
        });
        LayoutContent();
    }

private:
    void LayoutContent()
    {
        if (m_content == nullptr)
            return;
        const int inset = std::max(1, FromDIP(1));
        const wxSize size = GetClientSize();
        m_content->SetSize(inset, inset,
                           std::max(0, size.x - 2 * inset),
                           std::max(0, size.y - 2 * inset));
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const auto& colours = UiTheme::Palette();
        dc.SetBackground(wxBrush(colours.bgPanel));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxRect bounds = GetClientRect();
        // 复用"绿色框"（顶部工具条 chip）的颜色与形状：surfaceOnWindow 底 + 1px 描边 + 半高圆角。
        const double radius = bounds.height / 2.0;
        const wxColour fill = colours.surfaceOnWindow;
        gc->SetBrush(gc->CreateBrush(wxBrush(fill)));
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(colours.strokeOnWindow).Width(std::max(1, FromDIP(1)))));
        gc->DrawRoundedRectangle(0.5, 0.5,
                                 std::max(0, bounds.width - 1),
                                 std::max(0, bounds.height - 1), radius);
    }

    wxWindow* m_content = nullptr;
    bool m_readOnly = false;
    bool m_hover = false;
    bool m_focus = false;
    UiTheme::Subscription m_themeSubscription;
};

static void SetFieldMinimum(UiFieldChrome* shell, wxWindow* ctrl, wxWindow* parent)
{
    // 与顶部工具条 chip 取同一高度（ToolBarChipH），保证"完全一样的胶囊形状"。
    const int fieldH = parent->FromDIP(UiTheme::Skeleton::ToolBarChipH);
    wxSize minimum = ctrl->GetMinSize();
    minimum.y = fieldH;
    ctrl->SetMinSize(minimum);
    shell->SetMinSize(wxSize(-1, fieldH));
    shell->SetMaxSize(wxSize(-1, fieldH));
}

static wxTextCtrl* MakeTextField(wxWindow* parent, const wxString& value,
                                 long style, const wxPoint& pos, const wxSize& size,
                                 bool readOnly, UiFieldChrome** shellOut)
{
    auto* shell = new UiFieldChrome(parent);
    auto* ctrl = new wxTextCtrl(shell, wxID_ANY, value, pos, size,
                                style & ~wxBORDER_MASK);
    shell->SetContent(ctrl, readOnly);
    if (shellOut)
        *shellOut = shell;
    SetFieldMinimum(shell, ctrl, parent);
    return ctrl;
}

static wxChoice* MakeChoiceField(wxWindow* parent, const wxArrayString& choices,
                                 UiFieldChrome** shellOut)
{
    auto* shell = new UiFieldChrome(parent);
    auto* ctrl = new wxChoice(shell, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              choices, wxBORDER_NONE);
    shell->SetContent(ctrl, false);
    if (shellOut)
        *shellOut = shell;
    SetFieldMinimum(shell, ctrl, parent);
    return ctrl;
}

}  // namespace

// T-330：表单行取色/取字体一律经 UiTheme（SAFE-012 / THE-004），本文件不得出现颜色字面量。
// 字体档：label 11 DIP/Medium（text3）；输入控件文字 12 DIP（P12）。
static wxFont LabelFont(const wxWindow* parent) {
    return UiTheme::UiFont(parent, UiTheme::TextSize::P11, UiTheme::Weight::Medium);
}

static wxFont InputFont(const wxWindow* parent) {
    return UiTheme::UiFont(parent, UiTheme::TextSize::P12, UiTheme::Weight::Normal);
}

// CMP-055：28 DIP 为输入高度下限；保留既有宽度，并尊重系统字体/下拉箭头的原生最小高。
// 只在既有样式 helper 中设置，不改 LoadNode、端口回调或表达式业务。
static void ApplyInputHeight(wxWindow* ctrl, wxWindow* parent) {
    wxSize minimum = ctrl->GetMinSize();
    minimum.y = parent->FromDIP(UiTheme::Skeleton::ToolBarChipH);
    ctrl->SetMinSize(minimum);
}

// 辅助函数：设置只读文本框（CMP-055 只读项色弱：文字 text3 + 底 bgPanel 表示不可编辑）
static void SetReadOnlyBackground(wxTextCtrl* ctrl, wxWindow* parent) {
    ctrl->SetEditable(false);
    ctrl->SetBackgroundColour(UiTheme::Palette().surfaceOnWindow);
    ctrl->SetForegroundColour(UiTheme::Palette().text1);   // 统一为 text1（原来是 text3 偏灰）
    ctrl->SetFont(InputFont(parent));
    ApplyInputHeight(ctrl, parent);
}

// 可编辑输入控件：凹陷底（§3.5 合成实色 sunkenOnPanel，不用带 alpha 的 surfaceSunken）
static void ApplyInputLook(wxWindow* ctrl, wxWindow* parent) {
    ctrl->SetFont(InputFont(parent));
    ApplyInputHeight(ctrl, parent);
    ctrl->SetForegroundColour(UiTheme::Palette().text1);
    ctrl->SetBackgroundColour(UiTheme::Palette().surfaceOnWindow);
}

// 标签：11 DIP / Medium / text3
static void ApplyLabelLook(wxStaticText* label, wxWindow* parent) {
    label->SetFont(LabelFont(parent));
    label->SetForegroundColour(UiTheme::Palette().text3);
}

// 分隔线：1px strokeOnPanel
static wxStaticLine* MakeSeparator(wxWindow* parent) {
    auto* line = new wxStaticLine(parent, wxID_ANY);
    line->SetForegroundColour(UiTheme::Palette().strokeOnPanel);
    return line;
}

wxSizer* PropertyPanelBuilder::CreateSectionTitle(wxWindow* parent, const wxString& title) {
    auto vSizer = new wxBoxSizer(wxVERTICAL);
    auto label = new wxStaticText(parent, wxID_ANY, title);
    label->SetFont(LabelFont(parent));
    label->SetForegroundColour(UiTheme::Palette().text2); // 硬编码标题色 → text2
    vSizer->Add(label, 0, wxTOP | wxLEFT | wxRIGHT, 10);
    vSizer->Add(MakeSeparator(parent), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
    return vSizer;
}

// 图三：统一"字段行"骨架 —— label 固定列宽（灰字） + 右侧圆角输入壳 + 行末 1px 分隔线，
// 让各行输入框左侧成列对齐（原来 label 宽度随文字变化，输入框起始位置参差不齐）。
static wxSizer* WrapFieldRow(wxWindow* parent, wxStaticText* lbl, wxWindow* shell) {
    lbl->SetMinSize(wxSize(parent->FromDIP(88), -1));
    auto* h = new wxBoxSizer(wxHORIZONTAL);
    h->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(10));
    h->Add(shell, 1, wxEXPAND);
    auto* v = new wxBoxSizer(wxVERTICAL);
    v->Add(h, 0, wxEXPAND | wxTOP | wxBOTTOM, parent->FromDIP(7));
    v->Add(MakeSeparator(parent), 0, wxEXPAND);
    return v;
}

wxSizer* PropertyPanelBuilder::CreateTextRow(wxWindow* parent, const wxString& label, const wxString& value) {
    auto lbl = new wxStaticText(parent, wxID_ANY, label);
    ApplyLabelLook(lbl, parent);
    UiFieldChrome* shell = nullptr;
    auto txt = MakeTextField(parent, value, wxTE_READONLY, wxDefaultPosition,
                             wxDefaultSize, true, &shell);
    SetReadOnlyBackground(txt, parent);
    return WrapFieldRow(parent, lbl, shell);
}

wxSizer* PropertyPanelBuilder::CreateIdentifierRow(wxWindow* parent, const wxString& label,
    const wxString& initialValue, wxTextCtrl** textCtrlOut) {
    auto lbl = new wxStaticText(parent, wxID_ANY, label);
    ApplyLabelLook(lbl, parent);
    UiFieldChrome* shell = nullptr;
    auto txt = MakeTextField(parent, initialValue, wxTE_PROCESS_ENTER,
        wxDefaultPosition, wxDefaultSize, false, &shell);
    txt->SetEditable(true);
    ApplyInputLook(txt, parent);
    if (textCtrlOut) *textCtrlOut = txt;
    return WrapFieldRow(parent, lbl, shell);
}

wxSizer* PropertyPanelBuilder::CreateEditableTextRow(wxWindow* parent, const wxString& label,
    const wxString& initialValue, wxTextCtrl** textCtrlOut) {
    auto lbl = new wxStaticText(parent, wxID_ANY, label);
    ApplyLabelLook(lbl, parent);
    UiFieldChrome* shell = nullptr;
    auto txt = MakeTextField(parent, initialValue, wxTE_PROCESS_ENTER,
        wxDefaultPosition, wxDefaultSize, false, &shell);
    txt->SetEditable(true);
    ApplyInputLook(txt, parent);
    if (textCtrlOut) *textCtrlOut = txt;
    return WrapFieldRow(parent, lbl, shell);
}

wxSizer* PropertyPanelBuilder::CreateChoiceRow(wxWindow* parent, const wxString& label,
    const wxArrayString& choices, int initialSelection,
    wxChoice** choiceOut) {
    auto lbl = new wxStaticText(parent, wxID_ANY, label);
    ApplyLabelLook(lbl, parent);
    UiFieldChrome* shell = nullptr;
    auto ch = MakeChoiceField(parent, choices, &shell);
    ch->SetSelection(initialSelection);
    ApplyInputLook(ch, parent);
    if (choiceOut) *choiceOut = ch;
    return WrapFieldRow(parent, lbl, shell);
}

wxSizer* PropertyPanelBuilder::CreateTopPortRow(wxWindow* parent, const wxString& direction,
    const wxString& portName,
    wxTextCtrl** nameCtrlOut, wxButton** deleteBtnOut) {
    auto rowSizer = new wxBoxSizer(wxHORIZONTAL);
    UiFieldChrome* dirShell = nullptr;
    auto dirCtrl = MakeTextField(parent, direction, wxTE_READONLY,
                                 wxDefaultPosition, wxDefaultSize, true, &dirShell);
    SetReadOnlyBackground(dirCtrl, parent);
    rowSizer->Add(dirShell, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

    UiFieldChrome* nameShell = nullptr;
    auto nameCtrl = MakeTextField(parent, portName, wxTE_PROCESS_ENTER,
        wxDefaultPosition, wxDefaultSize, false, &nameShell);
    nameCtrl->SetEditable(true);
    ApplyInputLook(nameCtrl, parent);
    rowSizer->Add(nameShell, 1, wxEXPAND | wxLEFT, 10);
    if (nameCtrlOut) *nameCtrlOut = nameCtrl;

    auto delBtn = new wxButton(parent, wxID_ANY, "x", wxDefaultPosition, wxSize(25, 25));
    delBtn->SetToolTip("Delete this port");
    delBtn->SetFont(InputFont(parent));
    delBtn->SetBackgroundColour(UiTheme::Palette().strongOnPanel);
    delBtn->SetForegroundColour(UiTheme::Palette().text2);
    rowSizer->Add(delBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);
    if (deleteBtnOut) *deleteBtnOut = delBtn;

    // 整个行包装在垂直 sizer 中，以便添加分割线（由调用者添加）
    auto wrapper = new wxBoxSizer(wxVERTICAL);
    wrapper->Add(rowSizer, 0, wxEXPAND);
    wrapper->Add(MakeSeparator(parent), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
    return wrapper;
}

wxSizer* PropertyPanelBuilder::CreateSecondPortRow(wxWindow* parent, const wxString& direction,
    const wxString& portName, const wxString& connValue,
    const wxArrayString& choices, // 新增：信号选项列表
    wxChoice** connChoiceOut,     // 修改：返回 Choice 而不是 TextCtrl
    wxButton** deleteBtnOut)
{
    auto rowSizer = new wxBoxSizer(wxHORIZONTAL);
    UiFieldChrome* dirShell = nullptr;
    auto dirCtrl = MakeTextField(parent, direction, wxTE_READONLY,
                                 wxDefaultPosition, wxDefaultSize, true, &dirShell);
    SetReadOnlyBackground(dirCtrl, parent);
    rowSizer->Add(dirShell, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

    UiFieldChrome* nameShell = nullptr;
    auto nameCtrl = MakeTextField(parent, portName, wxTE_READONLY,
                                  wxDefaultPosition, wxDefaultSize, true, &nameShell);
    SetReadOnlyBackground(nameCtrl, parent);
    rowSizer->Add(nameShell, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);

    auto colon = new wxStaticText(parent, wxID_ANY, "connected by");
    colon->SetForegroundColour(UiTheme::Palette().text3);
    colon->SetFont(LabelFont(parent));
    rowSizer->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);

    UiFieldChrome* connShell = nullptr;
    auto connChoice = MakeChoiceField(parent, choices, &connShell);
    ApplyInputLook(connChoice, parent);

    int selection = connChoice->FindString(connValue);
    if (selection != wxNOT_FOUND) {
        connChoice->SetSelection(selection);
    }

    rowSizer->Add(connShell, 1, wxEXPAND | wxRIGHT, 5);
    if (connChoice) *connChoiceOut = connChoice;

    // 删除按钮
    if (deleteBtnOut) {
        auto delBtn = new wxButton(parent, wxID_ANY, "X", wxDefaultPosition, wxSize(25, 25));
        delBtn->SetToolTip("Delete this port");
        delBtn->SetFont(InputFont(parent));
        delBtn->SetBackgroundColour(UiTheme::Palette().strongOnPanel);
        delBtn->SetForegroundColour(UiTheme::Palette().text2);
        rowSizer->Add(delBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        *deleteBtnOut = delBtn;
    }

    auto wrapper = new wxBoxSizer(wxVERTICAL);
    wrapper->Add(rowSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 5);
    wrapper->Add(MakeSeparator(parent), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
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
    exprLabel->SetFont(LabelFont(parent));
    exprLabel->SetForegroundColour(UiTheme::Palette().text3);
    rowSizer->Add(exprLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // # 和 delay 文本框
    auto hash = new wxStaticText(parent, wxID_ANY, "#");
    hash->SetFont(LabelFont(parent));
    hash->SetForegroundColour(UiTheme::Palette().text3);
    rowSizer->Add(hash, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
    UiFieldChrome* delayShell = nullptr;
    auto delayCtrl = MakeTextField(parent, wxString::Format("%.1f", delay),
        wxTE_PROCESS_ENTER, wxDefaultPosition, wxSize(40, -1), false, &delayShell);
    ApplyInputLook(delayCtrl, parent);
    rowSizer->Add(delayShell, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    if (delayCtrlOut) *delayCtrlOut = delayCtrl;

    // 输出端口名（只读）
    UiFieldChrome* outShell = nullptr;
    auto outCtrl = MakeTextField(parent, outPortName, wxTE_READONLY,
                                 wxDefaultPosition, wxSize(60, -1), true, &outShell);
    SetReadOnlyBackground(outCtrl, parent);
    rowSizer->Add(outShell, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // 操作符选择
    wxArrayString choices;
    choices.Add("=");   // blocking
    choices.Add("<=");  // non-blocking
    UiFieldChrome* opShell = nullptr;
    auto opChoice = MakeChoiceField(parent, choices, &opShell);
    opChoice->SetSelection(isBlocking ? 0 : 1);
    ApplyInputLook(opChoice, parent);
    rowSizer->Add(opShell, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    if (opChoiceOut) *opChoiceOut = opChoice;

    // RHS 文本框
    UiFieldChrome* rhsShell = nullptr;
    auto rhsCtrl = MakeTextField(parent, rhs, wxTE_PROCESS_ENTER,
        wxDefaultPosition, wxDefaultSize, false, &rhsShell);
    rhsCtrl->SetHint("e.g. In1 & In2");
    rhsCtrl->SetMinSize(wxSize(300, -1));
    ApplyInputLook(rhsCtrl, parent);
    rowSizer->Add(rhsShell, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    if (rhsCtrlOut) *rhsCtrlOut = rhsCtrl;

    // 删除按钮
    if (deleteBtnOut) {
        auto delBtn = new wxButton(parent, wxID_ANY, "X", wxDefaultPosition, wxSize(25, 25));
        delBtn->SetToolTip("Delete this expression");
        delBtn->SetFont(InputFont(parent));
        delBtn->SetBackgroundColour(UiTheme::Palette().strongOnPanel);
        delBtn->SetForegroundColour(UiTheme::Palette().text2);
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
    btn->SetFont(InputFont(parent));
    if (foreground.IsOk())
        btn->SetForegroundColour(foreground);
    return btn;
}
