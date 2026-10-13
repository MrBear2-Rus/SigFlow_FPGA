#include "SFNPropertyPanel.h"
#include "PropertyPanelBuilder.h"
#include "platform/PlatformPaths.h"
#include "ui/UiTheme.h"
#include <wx/statline.h>

wxDEFINE_EVENT(EVT_SFTREE_CHANGED, wxCommandEvent);

// T-330：仅视觉 token 化（字体 + 颜色 + 只读态），不改动任何业务路径。
// 字体档：label 11 DIP/Medium（text3）；输入控件文字 12 DIP（P12）。
static wxFont PropertyLabelFont(const wxWindow* parent) {
    return UiTheme::UiFont(parent, UiTheme::TextSize::P11, UiTheme::Weight::Medium);
}

static wxFont PropertyInputFont(const wxWindow* parent) {
    return UiTheme::UiFont(parent, UiTheme::TextSize::P12, UiTheme::Weight::Normal);
}

// 本面板内自建的按钮/静态标签统一外观（文字档 + 中性底/前景）
static void ApplyPropertyPanelLook(wxWindow* wnd, const wxColour& fg) {
    wnd->SetFont(PropertyInputFont(wnd));
    wnd->SetBackgroundColour(UiTheme::Palette().strongOnPanel);
    wnd->SetForegroundColour(fg);
}

// ---------------------------------------------------------------------------
// T-140 / THE-030：本面板的按钮是**在 LoadNode 里即时创建**的，颜色只写一次，
// 主题切换后不会更新。为让 ReapplyTheme() 能无歧义地重取前景 token，
// 创建时把"语义角色"挂在按钮的 client data 上（wxButton::SetClientData 是
// 展示用途的挂点，与 wxEVT_BUTTON 事件绑定 / LoadNode 业务路径完全无关）。
// ---------------------------------------------------------------------------
enum : long {
    kButtonRoleNeutral = 1,  ///< Add/Delete Node 之类的默认按钮 → text1
    kButtonRoleAction  = 2,  ///< 主行动（+ Add Port / Add Expression）→ accent
    kButtonRoleDanger  = 3   ///< 删除语义（- Delete Port 等）→ danger
};

/// 无 client data 的按钮（PropertyPanelBuilder::CreateButton 直出的那些）按
/// 文案判定语义：`-`/Delete 系 → danger，其余 → accent（两者都是本文件既有取值）。
static wxColour ButtonForegroundFor(wxAnyButton* button, const UiTheme::Colours& colours) {
    if (button == nullptr) {
        return colours.text1;
    }
    void* role = button->GetClientData();
    if (role == reinterpret_cast<void*>(kButtonRoleDanger)) {
        return colours.danger;
    }
    if (role == reinterpret_cast<void*>(kButtonRoleAction)) {
        return colours.accent;
    }
    if (role == reinterpret_cast<void*>(kButtonRoleNeutral)) {
        return colours.text1;
    }
    const wxString label = button->GetLabel();
    if (label.StartsWith("-") || label.Contains("Delete")) {
        return colours.danger;
    }
    return colours.accent;
}

// 1px 分隔线：strokeOnPanel
static wxStaticLine* MakePropertySeparator(wxWindow* parent) {
    auto* line = new wxStaticLine(parent, wxID_ANY);
    line->SetForegroundColour(UiTheme::Palette().strokeOnPanel);
    return line;
}

// "Add Expression" / "Delete Last Expression" 底改为中性合成实色（THE-021），
// 删除语义改由 danger 前景色佐证（THE-024：状态不裸靠颜色）。
static void StylePropertyButton(wxButton* btn, bool danger) {
    ApplyPropertyPanelLook(btn, danger ? UiTheme::Palette().danger
                                       : UiTheme::Palette().text1);
    // T-140：登记语义角色 → ReapplyTheme() 能按 token 重取同一前景色。
    if (btn) {
        btn->SetClientData(reinterpret_cast<void*>(danger ? kButtonRoleDanger
                                                          : kButtonRoleNeutral));
    }
}

// 辅助函数：创建带前景色的添加按钮并绑定事件
static wxButton* CreateAddButton(wxWindow* parent, const wxString& label,
    std::function<void()> handler) {
    // 主辑：原色为饱和蓝 rgb(0,120,215)，对应 token 是 accent 本身。
    // 注意不要用焦点环合成色 —— 那是 accent 45% 与面板底合成后的**浅**蓝 (#87baee)，
    // 作为按钮文字会发白且对比度不足（THE-023）。
    auto btn = PropertyPanelBuilder::CreateButton(parent, label, UiTheme::Palette().accent);
    ApplyPropertyPanelLook(btn, UiTheme::Palette().accent);
    // T-140：主行动按钮的语义角色（重施时取 accent，不再依赖文案猜色）。
    btn->SetClientData(reinterpret_cast<void*>(kButtonRoleAction));
    btn->Bind(wxEVT_BUTTON, [handler](wxCommandEvent&) { handler(); });
    return btn;
}

SFNPropertyPanel::SFNPropertyPanel(wxWindow* parent, SigFlowTree* tree)
    : wxScrolledWindow(parent, wxID_ANY), m_tree(tree), m_node(nullptr),
    m_reloading(false), m_reloadRequested(false) {
    m_mainSizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(m_mainSizer);
    SetScrollRate(5, 5);
    SetTargetWindow(this);

    // T-140 / THE-030：构造末尾建立订阅 + 立即重施一次。此时表单尚未构建（LoadNode 为空
    // 操作），重施是幂等的自底色写入；真正的表单取色发生在 LoadNode，之后由本订阅重施。
    // 回调只做取色 + Refresh(false)：**不**调用 LoadNode()（那会 Layout/FitInside 并重建
    // 表单，违反"不重布局/不改业务状态"），也**不**调用 Layout/SetSize/SendSizeEvent。
    m_themeSubscription = UiTheme::Subscribe([this] {
        ReapplyTheme();
        Refresh(false);
    });
    ReapplyTheme();
}

// T-140 / THE-030：面板级主题重施入口（对外唯一入口）。
// 只递归重写**已存在**控件的 token 色，不重建表单、不改 LoadNode 路径/业务状态、
// 不做任何重布局。
void SFNPropertyPanel::ReapplyTheme()
{
    SetBackgroundColour(UiTheme::Palette().bgPanel);
    for (wxWindow* child : GetChildren()) {
        ReapplyTheme(child);
    }
}

// —— 按控件类型重施（父重载：递归下探子窗口）——
void SFNPropertyPanel::ReapplyTheme(wxWindow* window)
{
    if (window == nullptr) {
        return;
    }
    if (auto* button = dynamic_cast<wxButton*>(window)) {
        ReapplyTheme(button);
        return;
    }
    if (auto* text = dynamic_cast<wxTextCtrl*>(window)) {
        ReapplyTheme(text);
        return;
    }
    if (auto* label = dynamic_cast<wxStaticText*>(window)) {
        ReapplyTheme(label);
        return;
    }
    if (auto* line = dynamic_cast<wxStaticLine*>(window)) {
        ReapplyTheme(line);
        return;
    }
    if (auto* choice = dynamic_cast<wxChoice*>(window)) {
        ReapplyTheme(choice);
        return;
    }
    for (wxWindow* child : window->GetChildren()) {
        ReapplyTheme(child);
    }
}

// 按钮：中性底 strongOnPanel + 语义前景（accent / danger / text1）。
// 语义角色优先取 client data（CreateAddButton / StylePropertyButton 登记），
// 未登记的按钮（PropertyPanelBuilder 直出，如 "- Delete Port"）按文案判定，
// 两者都落在本文件既有的 token 取值上，不引入任何新颜色字面量。
void SFNPropertyPanel::ReapplyTheme(wxButton* button)
{
    if (button == nullptr) {
        return;
    }
    const UiTheme::Colours& colours = UiTheme::Palette();
    button->SetFont(PropertyInputFont(button));
    button->SetBackgroundColour(colours.strongOnPanel);
    button->SetForegroundColour(ButtonForegroundFor(button, colours));
    for (wxWindow* child : button->GetChildren()) {
        ReapplyTheme(child);
    }
}

// 文本框：可编辑 → text1 on sunkenOnPanel；只读 → text3 on bgPanel（与
// PropertyPanelBuilder 的 SetReadOnlyBackground / ApplyInputLook 同一规格）。
void SFNPropertyPanel::ReapplyTheme(wxTextCtrl* text)
{
    if (text == nullptr) {
        return;
    }
    const UiTheme::Colours& colours = UiTheme::Palette();
    text->SetFont(PropertyInputFont(text));
    if (text->IsEditable()) {
        text->SetBackgroundColour(colours.sunkenOnPanel);
        text->SetForegroundColour(colours.text1);
    } else {
        text->SetBackgroundColour(colours.bgPanel);
        text->SetForegroundColour(colours.text3);
    }
}

// 静态标签：text2 是行内正文（只读取值行）与章节标题，
// 而被 CreateTextRow 直接赋色的标签也统一取该 token。
void SFNPropertyPanel::ReapplyTheme(wxStaticText* label)
{
    if (label == nullptr) {
        return;
    }
    label->SetFont(PropertyLabelFont(label));
    label->SetForegroundColour(UiTheme::Palette().text2);
}

// 1px 分隔线：strokeOnPanel
void SFNPropertyPanel::ReapplyTheme(wxStaticLine* line)
{
    if (line == nullptr) {
        return;
    }
    line->SetForegroundColour(UiTheme::Palette().strokeOnPanel);
}

// 下拉框：与可编辑输入同一规格（text1 on sunkenOnPanel）
void SFNPropertyPanel::ReapplyTheme(wxChoice* choice)
{
    if (choice == nullptr) {
        return;
    }
    choice->SetFont(PropertyInputFont(choice));
    choice->SetForegroundColour(UiTheme::Palette().text1);
    choice->SetBackgroundColour(UiTheme::Palette().sunkenOnPanel);
    for (wxWindow* child : choice->GetChildren()) {
        ReapplyTheme(child);
    }
}

void SFNPropertyPanel::ClearForm() {
    if (m_mainSizer) {
        m_mainSizer->Clear(true);
    }
}

void SFNPropertyPanel::Fresh() {
    if (m_reloading) {
        m_reloadRequested = true;
        return;
    }
    m_reloading = true;
    Freeze();
    LoadNode(m_node);
    Thaw();
    m_reloading = false;
    if (m_reloadRequested) {
        m_reloadRequested = false;
        Fresh();
    }
}

void SFNPropertyPanel::Fresh_Self() {
    if (m_reloading) {
        m_reloadRequested = true;
        return;
    }
    m_reloading = true;
    Freeze();
    LoadNode(m_node);
    Thaw();
    wxPostEvent(this, wxCommandEvent());
    m_reloading = false;
    if (m_reloadRequested) {
        m_reloadRequested = false;
        Fresh();
    }
}

void SFNPropertyPanel::LoadNode(SigTreeNode* node) {
    if (!node) return;
    m_node = node;
    Freeze();
    ClearForm();

    switch (node->type) {
    case SigTreeNodeType::Project: {
        auto pn = static_cast<ProjectNode*>(node);
        //m_mainSizer->Add(PropertyPanelBuilder::CreateSectionTitle(this, "Project Properties"), 0, wxEXPAND | wxALL, 5);
        m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "Type:", "Project"), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "Project Path:", pn->projectPath), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        break;
    }
    case SigTreeNodeType::File: {
        auto fn = static_cast<FileNode*>(node);
        //m_mainSizer->Add(PropertyPanelBuilder::CreateSectionTitle(this, "File Properties"), 0, wxEXPAND | wxALL, 5);
        m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "Type:", "File"), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "File Path:", fn->filePath), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        break;
    }
    case SigTreeNodeType::Top: {
        auto tn = static_cast<TopNode*>(node);
        //m_mainSizer->Add(PropertyPanelBuilder::CreateSectionTitle(this, "Top Properties"), 0, wxEXPAND | wxALL, 5);
        m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "Type:", SigFlowTree::ToString(tn->topType)), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

        // Identifier
        wxTextCtrl* idCtrl = nullptr;
        m_mainSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(this, "Identifier: ", wxString::FromUTF8(tn->identifier), &idCtrl), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        if (idCtrl) {
            auto syncId = [this, tn](wxEvent& event) {
                wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
                if (!ctrl) { event.Skip(); return; }
                m_tree->ReIdentifier(tn, sigflow::platform::Utf8String(ctrl->GetValue()));
                if (event.GetEventType() == wxEVT_TEXT_ENTER)
                    this->GetParent()->SetFocus();
                event.Skip();
                };
            idCtrl->Bind(wxEVT_TEXT_ENTER, syncId);
            idCtrl->Bind(wxEVT_KILL_FOCUS, syncId);
        }

        // In ports
        m_mainSizer->Add(CreateTopPortsSizer(tn, PortDirection::In), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        // Out ports
        m_mainSizer->Add(CreateTopPortsSizer(tn, PortDirection::Out), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        break;
    }
    case SigTreeNodeType::Signal: {
        auto sn = static_cast<SignalNode*>(node);
        //m_mainSizer->Add(PropertyPanelBuilder::CreateSectionTitle(this, "Signal Properties"), 0, wxEXPAND | wxALL, 5);
        m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "Type:", SigFlowTree::ToString(sn->signalType)), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

        wxTextCtrl* idCtrl = nullptr;
        m_mainSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(this, "Identifier: ", wxString::FromUTF8(sn->identifier), &idCtrl), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        if (idCtrl) {
            auto syncId = [this, sn](wxEvent& event) {
                wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
                if (!ctrl) { event.Skip(); return; }
                m_tree->ReIdentifier(sn, sigflow::platform::Utf8String(ctrl->GetValue()));
                if (event.GetEventType() == wxEVT_TEXT_ENTER)
                    this->GetParent()->SetFocus();
                event.Skip();
                };
            idCtrl->Bind(wxEVT_TEXT_ENTER, syncId);
            idCtrl->Bind(wxEVT_KILL_FOCUS, syncId);
        }
        break;
    }
    case SigTreeNodeType::Second: {
        auto sn = static_cast<SecondNode*>(node);
        //m_mainSizer->Add(PropertyPanelBuilder::CreateSectionTitle(this, "Second Node Properties"), 0, wxEXPAND | wxALL, 5);
        m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "Type:", SigFlowTree::ToString(sn->secondType)), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

        if (sn->secondType == SecondNodeType::ModuleInstance) {
            auto min = static_cast<ModuleInstNode*>(sn);
            m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "Definition:", min->defIdentifier), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

            wxTextCtrl* idCtrl = nullptr;
            m_mainSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(this, "Identifier: ", wxString::FromUTF8(sn->identifier), &idCtrl), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            if (idCtrl) {
                auto syncId = [this, sn](wxEvent& event) {
                    wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
                    if (!ctrl) { event.Skip(); return; }
                    m_tree->ReIdentifier(sn, sigflow::platform::Utf8String(ctrl->GetValue()));
                    if (event.GetEventType() == wxEVT_TEXT_ENTER)
                        this->GetParent()->SetFocus();
                    event.Skip();
                    };
                idCtrl->Bind(wxEVT_TEXT_ENTER, syncId);
                idCtrl->Bind(wxEVT_KILL_FOCUS, syncId);
            }

            // 输入端口
            for (auto& p : sn->in_ports) {
                m_mainSizer->Add(MakePropertySeparator(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
                m_mainSizer->Add(CreateSecondPortRowSizer(sn, p.identifier, p.direction, p.conn), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            }
            // 输出端口
            for (auto& p : sn->out_ports) {
                m_mainSizer->Add(MakePropertySeparator(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
                m_mainSizer->Add(CreateSecondPortRowSizer(sn, p.identifier, p.direction, p.conn), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            }
        }
        else if (sn->secondType == SecondNodeType::GateInstance) {
            auto gin = static_cast<GateInstNode*>(sn);
            m_mainSizer->Add(PropertyPanelBuilder::CreateTextRow(this, "Gate Type:", SigFlowTree::ToString(gin->gatetype)), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

            wxTextCtrl* idCtrl = nullptr;
            m_mainSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(this, "Identifier: ", wxString::FromUTF8(sn->identifier), &idCtrl), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            if (idCtrl) {
                auto syncId = [this, sn](wxEvent& event) {
                    wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
                    if (!ctrl) { event.Skip(); return; }
                    m_tree->ReIdentifier(sn, sigflow::platform::Utf8String(ctrl->GetValue()));
                    if (event.GetEventType() == wxEVT_TEXT_ENTER)
                        this->GetParent()->SetFocus();
                    event.Skip();
                    };
                idCtrl->Bind(wxEVT_TEXT_ENTER, syncId);
                idCtrl->Bind(wxEVT_KILL_FOCUS, syncId);
            }

            for (auto& p : sn->in_ports) {
                m_mainSizer->Add(MakePropertySeparator(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
                m_mainSizer->Add(CreateSecondPortRowSizer(sn, p.identifier, p.direction, p.conn), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            }
            for (auto& p : sn->out_ports) {
                m_mainSizer->Add(MakePropertySeparator(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
                m_mainSizer->Add(CreateSecondPortRowSizer(sn, p.identifier, p.direction, p.conn), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            }
        }
        else if (sn->secondType == SecondNodeType::ContinuousAssign) {
            auto cn = static_cast<ContinuousAssignNode*>(sn);
            // 可编辑表达式行
            wxTextCtrl* expCtrl = nullptr;
            m_mainSizer->Add(PropertyPanelBuilder::CreateEditableTextRow(this, "Expression:", wxString::FromUTF8(cn->template_exp), &expCtrl), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            if (expCtrl) {
                expCtrl->SetClientData(&cn->template_exp);
                auto syncExp = [this](wxEvent& event) {
                    wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
                    if (!ctrl || !ctrl->GetClientData()) { event.Skip(); return; }
                    auto* dataPtr = static_cast<std::string*>(ctrl->GetClientData());
                    *dataPtr = sigflow::platform::Utf8String(ctrl->GetValue());
                    CallAfter([this]() { wxPostEvent(this, wxCommandEvent()); });
                    if (event.GetEventType() == wxEVT_TEXT_ENTER)
                        this->GetParent()->SetFocus();
                    event.Skip();
                    };
                expCtrl->Bind(wxEVT_TEXT_ENTER, syncExp);
                expCtrl->Bind(wxEVT_KILL_FOCUS, syncExp);
            }

            // 端口列表（含连接）
            m_mainSizer->Add(CreateContinuousAssignPortsSizer(cn), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        }
        else if (sn->secondType == SecondNodeType::Always) {
            auto an = static_cast<AlwaysNode*>(sn);
            // 复杂布局保留专用函数
            m_mainSizer->Add(Add_BN_OR_B_Expressions(an), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        }
        break;
    }
    }

    Layout();
    FitInside();
    Thaw();
}

// 创建 TopNode 端口列表（含添加按钮）
wxSizer* SFNPropertyPanel::CreateTopPortsSizer(TopNode* tn, PortDirection dir) {
    auto outerSizer = new wxBoxSizer(wxVERTICAL);
    std::vector<SignalNode*>& ports = (dir == PortDirection::In) ? tn->GetInPorts() : tn->GetOutPorts();
    wxString dirStr = SigFlowTree::ToString(dir);

    for (SignalNode* p : ports) {
        wxTextCtrl* nameCtrl = nullptr;
        wxButton* delBtn = nullptr;
        auto wrapper = PropertyPanelBuilder::CreateTopPortRow(this, dirStr, p->identifier, &nameCtrl, &delBtn);
        outerSizer->Add(wrapper, 0, wxEXPAND);

        // 绑定端口重命名
        if (nameCtrl) {
            wxString oldId = p->identifier;
            auto syncRename = [this, tn, oldId](wxEvent& event) {
                wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
                if (!ctrl) { event.Skip(); return; }
                m_tree->PortReName(tn, oldId, ctrl->GetValue());
                if (event.GetEventType() == wxEVT_TEXT_ENTER)
                    this->GetParent()->SetFocus();
                event.Skip();
                };
            nameCtrl->Bind(wxEVT_TEXT_ENTER, syncRename);
            nameCtrl->Bind(wxEVT_KILL_FOCUS, syncRename);
        }

        // 绑定删除端口
        if (delBtn) {
            delBtn->Bind(wxEVT_BUTTON, [this, tn, id = p->identifier](wxCommandEvent&) {
                m_tree->TopDelPort(tn, id);
                });
        }
    }

    // 添加端口按钮
    auto addBtn = CreateAddButton(this, "+ Add Port", [this, tn, dir]() {
        if (dir == PortDirection::In)
            m_tree->AddInPort(tn);
        else
            m_tree->AddOutPort(tn);
        LoadNode(m_node);  // 刷新
        });
    auto btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->Add(addBtn, 0, wxALL, 10);
    outerSizer->Add(btnSizer, 0, wxALIGN_CENTER);

    return outerSizer;
}

// 创建 SecondNode 的单端口行（含连接编辑）
wxSizer* SFNPropertyPanel::CreateSecondPortRowSizer(SecondNode* sn, const wxString& name,
    PortDirection dir, std::string& conn) {
    wxArrayString choices;
    choices.Add(""); // 允许空连接
    TopNode* t = static_cast<TopNode*>(sn->GetParent());
    for (auto& sig : t->signals) {
        choices.Add(wxString::FromUTF8(sig->identifier));
    }
    for (auto& sig : t->in_ports) {
        choices.Add(wxString::FromUTF8(sig->identifier));
    }
    for (auto& sig : t->out_ports) {
        choices.Add(wxString::FromUTF8(sig->identifier));
    }

    wxChoice* connChoice = nullptr;
    auto wrapper = PropertyPanelBuilder::CreateSecondPortRow(this, SigFlowTree::ToString(dir),
        name, wxString::FromUTF8(conn), choices ,&connChoice);
    if (connChoice) {
        wxString portName = name;
        connChoice->Bind(wxEVT_CHOICE, [this, sn, portName](wxCommandEvent& event) {
            wxChoice* choice = wxDynamicCast(event.GetEventObject(), wxChoice);
            if (choice) {
                std::string newVal = sigflow::platform::Utf8String(choice->GetStringSelection());

                // 调用后端逻辑更新连接
                m_tree->PortConn(sn, portName, newVal);

                // 触发面板刷新逻辑
                CallAfter([this]() {
                    wxPostEvent(this, wxCommandEvent(wxEVT_COMMAND_TEXT_UPDATED));
                    });
            }
            });
    }
    return wrapper;
}

// 创建 ContinuousAssignNode 的端口列表（含添加/删除按钮）
wxSizer* SFNPropertyPanel::CreateContinuousAssignPortsSizer(ContinuousAssignNode* cn) {
    auto outerSizer = new wxBoxSizer(wxVERTICAL);

    // 输出端口（只读连接，一般不添加删除）
    for (Port& p : cn->out_ports) {
        outerSizer->Add(CreateSecondPortRowSizer(cn, p.identifier, p.direction, p.conn), 0, wxEXPAND);
    }

    // 输入端口（可添加/删除）
    for (Port& p : cn->in_ports) {
        outerSizer->Add(CreateSecondPortRowSizer(cn, p.identifier, p.direction, p.conn), 0, wxEXPAND);
    }

    // 按钮组
    auto btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto addBtn = CreateAddButton(this, "+ Add Port", [this, cn]() {
        m_tree->AddInPort(cn);
        LoadNode(m_node);
        });
    auto delBtn = PropertyPanelBuilder::CreateButton(this, "- Delete Port", UiTheme::Palette().danger);
    delBtn->Bind(wxEVT_BUTTON, [this, cn](wxCommandEvent&) {
        if (!cn->in_ports.empty()) {
            m_tree->SecondDelLastInPort(cn);
            LoadNode(m_node);
        }
        else {
            wxLogWarning("Must keep at least one input port.");
        }
        });

    btnSizer->Add(addBtn, 0, wxALL, 5);
    btnSizer->Add(delBtn, 0, wxALL, 5);
    outerSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 15);

    return outerSizer;
}

// ==================== AlwaysNode 专用复杂布局（适配新结构）====================
wxSizer* SFNPropertyPanel::Add_BN_OR_B_Expressions(AlwaysNode* an) {
    auto mainSizer = new wxBoxSizer(wxVERTICAL);

    for (size_t i = 0; i < an->getStatementCount(); ++i) {
        const Statement* stmtBase = an->getStatement(i);
        auto* stmt = const_cast<AlwaysStatement*>(dynamic_cast<const AlwaysStatement*>(stmtBase));
        if (!stmt) continue;

        auto groupWrapper = new wxBoxSizer(wxVERTICAL);

        Add_BN_OR_B_Expression(groupWrapper, an, stmt, i);
        Add_BN_OR_B_Ports(groupWrapper, an, stmt, static_cast<int>(i));

        if (i < an->getStatementCount() - 1)
            groupWrapper->Add(MakePropertySeparator(this), 0, wxEXPAND | wxTOP | wxBOTTOM, 10);

        mainSizer->Add(groupWrapper, 0, wxEXPAND | wxALL, 10);
    }

    // 全局按钮（添加表达式、删除最后一个表达式）保持不变
    mainSizer->Add(MakePropertySeparator(this), 0, wxEXPAND | wxTOP | wxBOTTOM, 5);
    auto globalBtnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto addExpBtn = PropertyPanelBuilder::CreateButton(this, "Add Expression");
    StylePropertyButton(addExpBtn, false);
    auto delLastExpBtn = PropertyPanelBuilder::CreateButton(this, "Delete Last Expression");
    StylePropertyButton(delLastExpBtn, true);
    globalBtnSizer->Add(addExpBtn, 1, wxEXPAND | wxALL, 5);
    globalBtnSizer->Add(delLastExpBtn, 1, wxEXPAND | wxALL, 5);
    mainSizer->Add(globalBtnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    addExpBtn->Bind(wxEVT_BUTTON, [this, an](wxCommandEvent&) {
        an->AddEmptyExpression();
        LoadNode(m_node);
        });
    delLastExpBtn->Bind(wxEVT_BUTTON, [this, an](wxCommandEvent&) {
        an->DelLastExpression();
        an->CleanUnusedInPorts();
        LoadNode(m_node);
        });

    return mainSizer;
}

void SFNPropertyPanel::Add_BN_OR_B_Expression(wxSizer* groupSizer, AlwaysNode* an, AlwaysStatement* stmt, size_t expIndex) {
    wxString outName = wxString::FromUTF8(stmt->out_port_name);

    wxTextCtrl* delayCtrl = nullptr;
    wxChoice* opChoice = nullptr;
    wxTextCtrl* rhsCtrl = nullptr;
    wxButton* deleteExpBtn = nullptr;

    auto rowSizer = PropertyPanelBuilder::CreateNBOrBExpressionRow(this, stmt->delay, outName,
        stmt->is_blocking,
        wxString::FromUTF8(stmt->nb_or_b_expression),
        &delayCtrl, &opChoice, &rhsCtrl, &deleteExpBtn);
    groupSizer->Add(rowSizer, 0, wxEXPAND | wxBOTTOM, 8);

    if (delayCtrl) {
        delayCtrl->Bind(wxEVT_TEXT_ENTER, [this, stmt](wxCommandEvent& e) {
            double val;
            if (e.GetString().ToDouble(&val)) {
                stmt->delay = (float)val;
                CallAfter([this]() { wxPostEvent(this, wxCommandEvent()); });
            }
            e.Skip();
            });
        delayCtrl->Bind(wxEVT_KILL_FOCUS, [this, stmt](wxFocusEvent& e) {
            double val;
            if (e.GetEventObject() && static_cast<wxTextCtrl*>(e.GetEventObject())->GetValue().ToDouble(&val)) {
                stmt->delay = (float)val;
                CallAfter([this]() { wxPostEvent(this, wxCommandEvent()); });
            }
            e.Skip();
            });
    }

    if (opChoice) {
        opChoice->Bind(wxEVT_CHOICE, [this, stmt](wxCommandEvent& e) {
            stmt->is_blocking = (e.GetSelection() == 0);
            CallAfter([this]() { wxPostEvent(this, wxCommandEvent()); });
            });
    }

    if (rhsCtrl) {
        rhsCtrl->SetClientData(&stmt->nb_or_b_expression);
        auto syncRHS = [this](wxEvent& event) {
            wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
            if (ctrl && ctrl->GetClientData()) {
                auto* dataPtr = static_cast<std::string*>(ctrl->GetClientData());
                *dataPtr = sigflow::platform::Utf8String(ctrl->GetValue());
                CallAfter([this]() { wxPostEvent(this, wxCommandEvent()); });
            }
            event.Skip();
            };
        rhsCtrl->Bind(wxEVT_TEXT_ENTER, syncRHS);
        rhsCtrl->Bind(wxEVT_KILL_FOCUS, syncRHS);
    }

    // 绑定删除表达式按钮
    if (deleteExpBtn) {
        deleteExpBtn->Bind(wxEVT_BUTTON, [this, an, expIndex](wxCommandEvent&) {
            an->RemoveExpression(expIndex);
            LoadNode(m_node); // 刷新面板
            });
    }
}

void SFNPropertyPanel::Add_BN_OR_B_Ports(wxSizer* groupSizer, AlwaysNode* an, AlwaysStatement* stmt, int exp_id) {
    // 输出端口
    for (auto& p : an->out_ports) {
        if (p.identifier == stmt->out_port_name) {
            Add_BN_OR_B_Port(groupSizer, an, p);
            break;
        }
    }

    // 输入端口
    for (const auto& name : stmt->in_port_names) {
        for (auto& p : an->in_ports) {
            if (p.identifier == name) {
                Add_BN_OR_B_Port(groupSizer, an, p);
                break;
            }
        }
    }

    // 只保留“+ Add Port”按钮
    auto btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto addBtn = CreateAddButton(this, "+ Add Port", [this, an, stmt]() {
        an->AddPortToExpression(stmt);
        LoadNode(m_node);
        });
    btnSizer->Add(addBtn, 0, wxALL, 5);
    groupSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 15);
}

void SFNPropertyPanel::Add_BN_OR_B_Port(wxSizer* groupSizer, AlwaysNode* an, Port& p)
{

    wxArrayString choices;
    choices.Add(""); // 允许空连接
    TopNode* t = static_cast<TopNode*>(an->GetParent());
    for (auto& sig : t->signals) {
        choices.Add(wxString::FromUTF8(sig->identifier));
    }
    for (auto& sig : t->in_ports) {
        choices.Add(wxString::FromUTF8(sig->identifier));
    }
    for (auto& sig : t->out_ports) {
        choices.Add(wxString::FromUTF8(sig->identifier));
    }

    wxChoice* connCtrl = nullptr;
    wxButton* deletePortBtn = nullptr;
    auto wrapper = PropertyPanelBuilder::CreateSecondPortRow(this, SigFlowTree::ToString(p.direction),
        p.identifier, wxString::FromUTF8(p.conn), choices,
        &connCtrl, &deletePortBtn);
    if (connCtrl) {
        wxString portName = p.identifier;
        connCtrl->Bind(wxEVT_CHOICE, [this, an, portName](wxCommandEvent& event) {
            wxChoice* choice = wxDynamicCast(event.GetEventObject(), wxChoice);
            if (choice) {
                std::string newVal = sigflow::platform::Utf8String(choice->GetStringSelection());

                // 调用后端逻辑更新连接
                m_tree->PortConn(an, portName, newVal);

                // 触发面板刷新逻辑
                CallAfter([this]() {
                    wxPostEvent(this, wxCommandEvent(wxEVT_COMMAND_TEXT_UPDATED));
                    });
            }
            });
    }

    // 绑定删除端口按钮
    if (deletePortBtn) {
        wxString portName = p.identifier;
        deletePortBtn->Bind(wxEVT_BUTTON, [this, an, portName](wxCommandEvent&) {
            // 调用按名称删除端口的方法
            an->DeletePort(sigflow::platform::Utf8String(portName));
            LoadNode(m_node);
            });
    }

    groupSizer->Add(wrapper, 0, wxEXPAND);
}
