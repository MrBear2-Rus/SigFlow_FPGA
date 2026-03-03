
#include "SFNPropertyPanel.h"
#include <wx/statline.h>

SFNPropertyPanel::SFNPropertyPanel(wxWindow* parent, SigFlowTree* tree) : wxScrolledWindow(parent, wxID_ANY) {
    m_mainSizer = new wxBoxSizer(wxVERTICAL);
    m_formSizer = nullptr; // 初始为空，由 ClearForm 创建
    m_tree = tree;
    SetSizer(m_mainSizer);
    SetScrollRate(5, 5);

    // 这一步很关键：确保内容过大时自动计算虚拟尺寸
    SetTargetWindow(this);
}

void SFNPropertyPanel::Fresh() {
    if (m_reloading) {
        // 如果当前就在 LoadNode 中，标记稍后再来一次
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
        // 如果当前就在 LoadNode 中，标记稍后再来一次
        m_reloadRequested = true;
        return;
    }
    m_reloading = true;

    Freeze();
    LoadNode(m_node);
    Thaw();

    wxCommandEvent evt;
    wxPostEvent(this, evt);

    m_reloading = false;

    if (m_reloadRequested) {
        m_reloadRequested = false;
        Fresh();
    }
}

void SFNPropertyPanel::LoadNode(SigTreeNode* node) {
    if (!node) return;
    m_node = node;
    this->Freeze();
    ClearForm();

    switch (node->type) {
    case SigTreeNodeType::Project:
    {
        AddTextRow("Type:", "Project");
        AddTextRow("Project Path:", static_cast<ProjectNode*>(node)->projectPath);
    }
    break;

    case SigTreeNodeType::File:

        AddTextRow("Type:", "File");
        AddTextRow("File Path:", static_cast<FileNode*>(node)->filePath);

        break;

    case SigTreeNodeType::Top:
    {
        auto t = static_cast<TopNode*>(node);
        AddTextRow("Type:", SigFlowTree::ToString(t->topType));
        AddIdentifier(static_cast<TopNode*>(node));

        AddPortRow(t, PortDirection::In);
        AddPortRow(t, PortDirection::Out);
        

    }
    break;

    case SigTreeNodeType::Signal:
    {
        auto sig = static_cast<SignalNode*>(node);
        AddTextRow("Type:", SigFlowTree::ToString(sig->signalType));
        AddIdentifier(sig);
    }
    break;

    case SigTreeNodeType::Second:
    {
        auto s = static_cast<SecondNode*>(node);
        AddTextRow("Type:", SigFlowTree::ToString(s->secondType));
        


        if (s->secondType == SecondNodeType::ModuleInstance) {
        
            AddTextRow("Definition: ", static_cast<ModuleInstNode*>(s)->defIdentifier);
            AddIdentifier(s);
            for (auto& p : s->in_ports) {
                AddPortRowWithConn(s, p.identifier, p.direction, p.conn);
            }
            for (auto& p : s->out_ports) {
                AddPortRowWithConn(s, p.identifier, p.direction, p.conn);
            }
        }
        if (s->secondType == SecondNodeType::GateInstance) {
            AddTextRow("Gate Type: ", SigFlowTree::ToString(static_cast<GateInstNode*>(s)->gatetype));
            AddIdentifier(s);
            for (auto& p : s->in_ports) {
                AddPortRowWithConn(s, p.identifier, p.direction, p.conn);
            }
            for (auto& p : s->out_ports) {
                AddPortRowWithConn(s, p.identifier, p.direction, p.conn);
            }
        }

        
        if (s->secondType == SecondNodeType::ContinuousAssign) {
           AddChangeTextRow("Expression:", static_cast<ContinuousAssignNode*>(s)->template_exp);
           AddPortContinuousAssign(static_cast<ContinuousAssignNode*>(s));
        }
            


        if (s->secondType == SecondNodeType::Always) {
            Add_BN_OR_B_Expressions(static_cast<AlwaysNode*>(s));
        }
    }
    break;
    }

    this->Layout();
    this->Thaw();

    m_mainSizer->Layout();
    FitInside();
}

void SFNPropertyPanel::ClearForm() {
    if (m_mainSizer) {
        m_mainSizer->Clear(true); // 删除所有旧控件
    }
    else {
        // 如果这里崩溃，说明构造函数没创建 m_mainSizer
        m_mainSizer = new wxBoxSizer(wxVERTICAL);
        SetSizer(m_mainSizer);
    }
    // 重新创建二列布局
    m_formSizer = new wxFlexGridSizer(2, 5, 10); // 2列，行间距5，列间距10
    m_formSizer->AddGrowableCol(1, 1); // 让第二列（输入框）自动拉伸
    m_mainSizer->Add(m_formSizer, 1, wxEXPAND | wxALL, 10);
}

void SFNPropertyPanel::AddTextRow(const wxString& label, const wxString& value) {
    m_formSizer->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    wxTextCtrl* txt = new wxTextCtrl(this, wxID_ANY, value);
    txt->SetEditable(false);
    m_formSizer->Add(txt, 1, wxEXPAND);
}

void SFNPropertyPanel::AddIdentifier(TopNode* tn) {
    m_formSizer->Add(new wxStaticText(this, wxID_ANY, "Identifier: "), 0, wxALIGN_CENTER_VERTICAL);
    wxTextCtrl* txt = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(tn->identifier), wxDefaultPosition, wxDefaultSize,
        wxTE_PROCESS_ENTER);
    txt->SetEditable(true);

    auto syncFunc = [this, tn](wxEvent& event) {
        wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
        if (!ctrl) {
            event.Skip();
            return;
        }

        m_tree->ReIdentifier(tn, ctrl->GetValue().ToStdString());
        
        if (event.GetEventType() == wxEVT_TEXT_ENTER) {
            this->GetParent()->SetFocus();
        }

        event.Skip();
        };

    txt->Bind(wxEVT_TEXT_ENTER, syncFunc);

    txt->Bind(wxEVT_KILL_FOCUS, syncFunc);

    m_formSizer->Add(txt, 1, wxEXPAND);
}

void SFNPropertyPanel::AddIdentifier(SecondNode* sn) {
    m_formSizer->Add(new wxStaticText(this, wxID_ANY, "Identifier: "), 0, wxALIGN_CENTER_VERTICAL);
    wxTextCtrl* txt = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(sn->identifier), wxDefaultPosition, wxDefaultSize,
        wxTE_PROCESS_ENTER);
    txt->SetEditable(true);

    auto syncFunc = [this, sn](wxEvent& event) {
        wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
        if (!ctrl) {
            event.Skip();
            return;
        }

        m_tree->ReIdentifier(sn, ctrl->GetValue().ToStdString());

        if (event.GetEventType() == wxEVT_TEXT_ENTER) {
            this->GetParent()->SetFocus();
        }

        event.Skip();
        };

    txt->Bind(wxEVT_TEXT_ENTER, syncFunc);

    txt->Bind(wxEVT_KILL_FOCUS, syncFunc);

    m_formSizer->Add(txt, 1, wxEXPAND);
}

void SFNPropertyPanel::AddIdentifier(SignalNode* sn) {
    m_formSizer->Add(new wxStaticText(this, wxID_ANY, "Identifier: "), 0, wxALIGN_CENTER_VERTICAL);
    wxTextCtrl* txt = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(sn->identifier), wxDefaultPosition, wxDefaultSize,
        wxTE_PROCESS_ENTER);
    txt->SetEditable(true);

    auto syncFunc = [this, sn](wxEvent& event) {
        wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
        if (!ctrl) {
            event.Skip();
            return;
        }

        m_tree->ReIdentifier(sn, ctrl->GetValue().ToStdString());

        if (event.GetEventType() == wxEVT_TEXT_ENTER) {
            this->GetParent()->SetFocus();
        }

        event.Skip();
        };

    txt->Bind(wxEVT_TEXT_ENTER, syncFunc);

    txt->Bind(wxEVT_KILL_FOCUS, syncFunc);

    m_formSizer->Add(txt, 1, wxEXPAND);
}

void SFNPropertyPanel::AddChangeTextRow(const wxString& label, std::string& value) {
    m_formSizer->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    wxTextCtrl* txt = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(value), wxDefaultPosition, wxDefaultSize,
        wxTE_PROCESS_ENTER);
    txt->SetEditable(true);
    txt->SetClientData(&value);

    auto syncFunc = [this](wxEvent& event) {
        wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
        if (!ctrl) {
            event.Skip();
            return;
        }

        auto* dataPtr = static_cast<std::string*>(ctrl->GetClientData());

        if (dataPtr) {
            *dataPtr = ctrl->GetValue().ToStdString();
        }

        // 不在当前事件栈中触发 LoadNode 或 UI 重建
        CallAfter([this]() {
            wxCommandEvent evt;
            wxPostEvent(this, evt);
            });

        if (event.GetEventType() == wxEVT_TEXT_ENTER) {
            this->GetParent()->SetFocus();
        }

        event.Skip();
        };

    txt->Bind(wxEVT_TEXT_ENTER, syncFunc);

    txt->Bind(wxEVT_KILL_FOCUS, syncFunc);

    m_formSizer->Add(txt, 1, wxEXPAND);


}











void SFNPropertyPanel::AddSectionTitle(const wxString& title) {
    // 1. 创建一个容器 Panel 或者 Sizer 来包裹标题和线
    // 这里我们直接加到 m_mainSizer（主垂直布局）中，因为它不需要分两列

    // 创建文字并加粗
    wxStaticText* label = new wxStaticText(this, wxID_ANY, title);
    wxFont font = label->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    label->SetFont(font);
    label->SetForegroundColour(wxColour(0, 102, 204)); // 给个深蓝色，显得专业

    // 创建水平分割线
    wxStaticLine* line = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);

    // 将标题和线加入主布局
    // 注意：我们要把它们加在 m_formSizer 之前或之后
    m_mainSizer->Add(label, 0, wxTOP | wxLEFT | wxRIGHT, 10);
    m_mainSizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
}




void SFNPropertyPanel::AddPortRow(TopNode* tn, PortDirection pd) {
    std::vector<Port>* ps = nullptr;
    if (pd == PortDirection::In) {
        ps = &(tn->GetInPorts());
    }
    else {
        ps = &(tn->GetOutPorts());
    }

    if (!ps) return;
    for (Port& p : *ps) {
        wxString name = p.identifier;
        PortDirection dir = p.direction;

        // 1. 创建水平布局行
        wxBoxSizer* rowSizer = new wxBoxSizer(wxVERTICAL);

        wxStaticLine* line = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
        
        wxBoxSizer* cSizer = new wxBoxSizer(wxHORIZONTAL);


        // --- 方向选择 ---
        wxTextCtrl* dirCtrl = new wxTextCtrl(this, wxID_ANY, SigFlowTree::ToString(pd),
            wxDefaultPosition, wxDefaultSize);
        dirCtrl->SetEditable(false);


        // --- 引脚名 (Port Name) ---
        wxTextCtrl* editName = new wxTextCtrl(this, wxID_ANY, name,
            wxDefaultPosition, wxDefaultSize,
            wxTE_PROCESS_ENTER);
        editName->SetHint("Port Name"); // 设置提示文字
        wxString old_id = p.identifier;
        auto syncFunc = [this, tn, old_id](wxEvent& event) {
            wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
            if (!ctrl) {
                event.Skip();
                return;
            }

            auto* dataPtr = static_cast<std::string*>(ctrl->GetClientData());

            if (dataPtr) {
                *dataPtr = ctrl->GetValue().ToStdString();
            }

            m_tree->PortReName(tn, old_id, ctrl->GetValue());

            if (event.GetEventType() == wxEVT_TEXT_ENTER) {
                this->GetParent()->SetFocus();
            }

            event.Skip();
            };

        editName->Bind(wxEVT_TEXT_ENTER, syncFunc);
        editName->Bind(wxEVT_KILL_FOCUS, syncFunc);



        // 3. 组合行布局 (比例分配：Name 占 1, Conn 占 1)
        wxButton* delBtn = new wxButton(this, wxID_ANY, "x", wxDefaultPosition, wxSize(25, 25));
        delBtn->SetToolTip("Delete this port");
        wxString id = p.identifier;
        delBtn->Bind(wxEVT_BUTTON,
            [this, tn, id](wxCommandEvent& event) {
                m_tree->TopDelPort(tn, id);
            }
        );


        cSizer->Add(dirCtrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
        cSizer->Add(editName, 1, wxEXPAND | wxLEFT, 10);
        cSizer->Add(delBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);
        rowSizer->Add(cSizer);
        rowSizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
        m_mainSizer->Add(rowSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);
    }

    wxButton* addBtn = new wxButton(this, wxID_ANY, "+ Add Port", wxDefaultPosition, wxDefaultSize);
    addBtn->SetForegroundColour(wxColour(0, 120, 215)); // 可选：设置为蓝色，增加视觉识别度

    // 2. 布局：可以居中或者靠右
    wxBoxSizer* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->Add(addBtn, 0, wxALL, 10); // 增加外边距

    // 3. 添加到主布局
    m_mainSizer->Add(btnSizer, 0, wxALIGN_CENTER); // 居中显示

    // 4. 绑定点击事件
    addBtn->Bind(wxEVT_BUTTON, [this, tn, pd](wxCommandEvent&) {
        if (pd == PortDirection::In) m_tree->AddInPort(tn);
        else if (pd == PortDirection::Out) m_tree->AddOutPort(tn);
        this->LoadNode(m_node);
        });


}

void SFNPropertyPanel::AddPortContinuousAssign(ContinuousAssignNode* cn) {
    std::vector<Port>& in_ps = cn->in_ports;
    std::vector<Port>& out_ps = cn->out_ports;
    for (Port& p : out_ps) {
        // --- 1. 每一项最外层的垂直包装 (放内容行 + 下方的线) ---
        wxBoxSizer* itemWrapper = new wxBoxSizer(wxVERTICAL);

        // --- 2. 核心：创建水平行 (让所有控件排成一排) ---
        wxBoxSizer* contentRow = new wxBoxSizer(wxHORIZONTAL);

        // --- 方向 & 名称容器 ---
        wxTextCtrl* dirC = new wxTextCtrl(this, wxID_ANY, SigFlowTree::ToString(p.direction));
        dirC->SetEditable(false);
        dirC->SetBackgroundColour(this->GetBackgroundColour());

        wxTextCtrl* editName = new wxTextCtrl(this, wxID_ANY, p.identifier);
        editName->SetEditable(false);
        editName->SetBackgroundColour(this->GetBackgroundColour());

        // --- 标签 & 输入框 ---
        wxStaticText* colon = new wxStaticText(this, wxID_ANY, "connected by");
        colon->SetForegroundColour(wxColour(120, 120, 120));

        wxTextCtrl* editConn = new wxTextCtrl(this, wxID_ANY, p.conn, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        editConn->SetHint("Connected Signal");
        editConn->SetBackgroundColour(wxColour(240, 248, 255));
        editConn->SetClientData(&p.conn);

        wxString id = p.identifier;
        auto syncFunc = [this, cn, id](wxEvent& event) {
            wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
            if (!ctrl) {
                event.Skip();
                return;
            }

            m_tree->PortConn(cn, id, ctrl->GetValue().ToStdString()) ;

            // 不在当前事件栈中触发 LoadNode 或 UI 重建
            CallAfter([this]() {
                wxCommandEvent evt;
                wxPostEvent(this, evt);
                });

            if (event.GetEventType() == wxEVT_TEXT_ENTER) {
                this->GetParent()->SetFocus();
            }

            event.Skip();
            };

        editConn->Bind(wxEVT_TEXT_ENTER, syncFunc);
        editConn->Bind(wxEVT_KILL_FOCUS, syncFunc);

        // --- 3. 核心布局修正：水平行里使用垂直居中是合法的 ---
        contentRow->Add(dirC, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
        contentRow->Add(editName, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
        contentRow->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
        contentRow->Add(editConn, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

        // --- 4. 组装：把水平行和线加进包装器 ---
        itemWrapper->Add(contentRow, 0, wxEXPAND | wxTOP | wxBOTTOM, 5);
        itemWrapper->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

        // --- 5. 把包装器加进主面板 ---
        m_mainSizer->Add(itemWrapper, 0, wxEXPAND);
    }
    for (Port& p : in_ps) {
        // --- 1. 每一项最外层的垂直包装 (放内容行 + 下方的线) ---
        wxBoxSizer* itemWrapper = new wxBoxSizer(wxVERTICAL);

        // --- 2. 核心：创建水平行 (让所有控件排成一排) ---
        wxBoxSizer* contentRow = new wxBoxSizer(wxHORIZONTAL);

        // --- 方向 & 名称容器 ---
        wxTextCtrl* dirC = new wxTextCtrl(this, wxID_ANY, SigFlowTree::ToString(p.direction));
        dirC->SetEditable(false);
        dirC->SetBackgroundColour(this->GetBackgroundColour());

        wxTextCtrl* editName = new wxTextCtrl(this, wxID_ANY, p.identifier);
        editName->SetEditable(false);
        editName->SetBackgroundColour(this->GetBackgroundColour());

        // --- 标签 & 输入框 ---
        wxStaticText* colon = new wxStaticText(this, wxID_ANY, "connected by");
        colon->SetForegroundColour(wxColour(120, 120, 120));

        wxTextCtrl* editConn = new wxTextCtrl(this, wxID_ANY, p.conn, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        editConn->SetHint("Connected Signal");
        editConn->SetBackgroundColour(wxColour(240, 248, 255));
        wxString id = p.identifier;
        auto syncFunc = [this, cn, id](wxEvent& event) {
            wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
            if (!ctrl) {
                event.Skip();
                return;
            }

            m_tree->PortConn(cn, id, ctrl->GetValue().ToStdString());

            // 不在当前事件栈中触发 LoadNode 或 UI 重建
            CallAfter([this]() {
                wxCommandEvent evt;
                wxPostEvent(this, evt);
                });

            if (event.GetEventType() == wxEVT_TEXT_ENTER) {
                this->GetParent()->SetFocus();
            }

            event.Skip();
            };

        editConn->Bind(wxEVT_TEXT_ENTER, syncFunc);
        editConn->Bind(wxEVT_KILL_FOCUS, syncFunc);

        // --- 3. 核心布局修正：水平行里使用垂直居中是合法的 ---
        contentRow->Add(dirC, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
        contentRow->Add(editName, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
        contentRow->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
        contentRow->Add(editConn, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

        // --- 4. 组装：把水平行和线加进包装器 ---
        itemWrapper->Add(contentRow, 0, wxEXPAND | wxTOP | wxBOTTOM, 5);
        itemWrapper->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

        // --- 5. 把包装器加进主面板 ---
        m_mainSizer->Add(itemWrapper, 0, wxEXPAND);
    }

    // --- 底部按钮组 ---
    wxBoxSizer* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton* addBtn = new wxButton(this, wxID_ANY, "+ Add Port", wxDefaultPosition, wxDefaultSize);
    addBtn->SetForegroundColour(wxColour(0, 120, 215)); // 可选：设置为蓝色，增加视觉识别度

    addBtn->Bind(wxEVT_BUTTON, [this, cn, &in_ps](wxCommandEvent&) {
        this->m_tree->AddInPort(cn);
        this->LoadNode(m_node);
        });

    wxButton* delBtn = new wxButton(this, wxID_ANY, "- Delete Port", wxDefaultPosition, wxDefaultSize);
    delBtn->SetForegroundColour(wxColour(200, 0, 0)); // 可选：设置为蓝色，增加视觉识别度

    // 4. 绑定点击事件
    delBtn->Bind(wxEVT_BUTTON, [this, cn, &in_ps](wxCommandEvent&) {


        if (in_ps.size() > 0) {
            m_tree->SecondDelLastInPort(cn);
        }
        else {
            wxLogWarning("Must keep at least output port.");
        }

        this->LoadNode(m_node);
        });

    btnSizer->Add(addBtn, 0, wxALL, 5);
    btnSizer->Add(delBtn, 0, wxALL, 5);

    // 再次提醒：m_mainSizer 是垂直的，这里只能用水平居中标志！
    m_mainSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 15);

    this->Layout();
}
void SFNPropertyPanel::AddPortRowWithConn(SecondNode* sn, const wxString& name, PortDirection dir, std::string& conn) {
    // 1. 创建分割线
    wxStaticLine* line = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
    m_mainSizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

    // 2. 创建水平布局行
    wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);


    wxTextCtrl* choice = new wxTextCtrl(this, wxID_ANY, SigFlowTree::ToString(dir));
    choice->SetEditable(false);


    // --- 引脚名 (Port Name) ---
    wxTextCtrl* editName = new wxTextCtrl(this, wxID_ANY, name);
    editName->SetEditable(false);

    // --- 链接符 ":" ---
    wxStaticText* colon = new wxStaticText(this, wxID_ANY, "connected by");

    // --- 连接的信号 (Connection / Signal) ---
    wxTextCtrl* editConn = new wxTextCtrl(this, wxID_ANY, conn,
        wxDefaultPosition, wxDefaultSize,
        wxTE_PROCESS_ENTER);
    editConn->SetHint("Connected Signal");
    // 设置一个淡蓝色背景或边框，视觉上区分“内部引脚”和“外部连线”
    editConn->SetBackgroundColour(wxColour(240, 248, 255));
    wxString id = name;
    auto syncFunc = [this, sn, id](wxEvent& event) {
        wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
        if (!ctrl) {
            event.Skip();
            return;
        }

        m_tree->PortConn(sn, id, ctrl->GetValue().ToStdString());

        // 不在当前事件栈中触发 LoadNode 或 UI 重建
        CallAfter([this]() {
            wxCommandEvent evt;
            wxPostEvent(this, evt);
            });

        if (event.GetEventType() == wxEVT_TEXT_ENTER) {
            this->GetParent()->SetFocus();
        }

        event.Skip();
        };

    editConn->Bind(wxEVT_TEXT_ENTER, syncFunc);
    editConn->Bind(wxEVT_KILL_FOCUS, syncFunc);


    // 3. 组合行布局 (比例分配：Name 占 1, Conn 占 1)
    rowSizer->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
    rowSizer->Add(editName, 1, wxEXPAND | wxLEFT, 5);
    rowSizer->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
    rowSizer->Add(editConn, 1, wxEXPAND | wxRIGHT, 10);

    m_mainSizer->Add(rowSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);
}






void SFNPropertyPanel::AddChoicesRow(const wxString& label,
    std::string& boundValue,
    const wxArrayString& choices)
{
    // Label
    auto* text = new wxStaticText(this, wxID_ANY, label);
    m_formSizer->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    // Choice
    auto* choice = new wxChoice(this, wxID_ANY,
        wxDefaultPosition,
        wxDefaultSize,
        choices);
    m_formSizer->Add(choice, 1, wxEXPAND);

    // ---- Model → UI ----
    int idx = choices.Index(boundValue);
    if (idx != wxNOT_FOUND) {
        choice->SetSelection(idx);
    }
    else if (!choices.IsEmpty()) {
        choice->SetSelection(0);
        boundValue = choices[0].ToStdString();
    }

    // ---- UI → Model ----
    choice->Bind(wxEVT_CHOICE, [this, choice, &boundValue](wxCommandEvent&) {
        boundValue = choice->GetStringSelection().ToStdString();

        CallAfter([this]() {
            wxCommandEvent evt(this->GetId());
            wxPostEvent(this, evt);
            });
        });
}

void SFNPropertyPanel::Add_BN_OR_B_Expression(wxSizer* groupSizer, std::vector<Port>& out_ports, NB_OR_B_Expression& exp) {
    // 1. 左侧 Label
    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(this, wxID_ANY, "Expression:"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

    // 2. 右侧容器 (水平排列所有控件)
    wxBoxSizer* hSizer = new wxBoxSizer(wxHORIZONTAL);

    // --- [num] Delay 控件 ---
    wxStaticText* hashSign = new wxStaticText(this, wxID_ANY, "#");
    hSizer->Add(hashSign, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

    wxTextCtrl* delayCtrl = new wxTextCtrl(this, wxID_ANY, wxString::Format("%.1f", exp.delay),
        wxDefaultPosition, wxSize(40, -1),
        wxTE_PROCESS_ENTER);
    hSizer->Add(delayCtrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // --- [out] Output 端口 (不可修改) ---
    // 通过索引从 ports 获取名称
    wxString outName = "??";

    outName = wxString::FromUTF8(out_ports[exp.out_port_id].identifier);

    wxTextCtrl* outCtrl = new wxTextCtrl(this, wxID_ANY, outName, wxDefaultPosition, wxSize(60, -1));
    outCtrl->SetEditable(false);
    hSizer->Add(outCtrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // --- [op] 操作符选择 (Blocking vs Non-blocking) ---
    wxArrayString choices;
    choices.Add("=");  // Blocking
    choices.Add("<="); // Non-blocking
    wxChoice* opChoice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    opChoice->SetSelection(exp.is_blocking ? 0 : 1);
    hSizer->Add(opChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // --- [rhs] 右值文本框 ---
    wxTextCtrl* rhsCtrl = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(exp.nb_or_b_expression),
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    rhsCtrl->SetHint("e.g. In1 & In2");
    rhsCtrl->SetMinSize(wxSize(300, -1));
    hSizer->Add(rhsCtrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // 3. 将水平组合放入主表单 Sizer
    row->Add(hSizer, 1, wxEXPAND);

    // --- 4. 数据绑定与反向更新逻辑 ---

    // 延迟修改绑定 (exp.delay_value)
    auto syncDelay = [this, &exp](wxEvent& event) {
        wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
        if (ctrl) {
            double val;
            // 尝试转换输入值
            if (ctrl->GetValue().ToDouble(&val)) {
                exp.delay = (float)val;
                // 触发树变更事件，通知其他组件更新
                this->CallAfter([this]() {
                    wxPostEvent(this, wxCommandEvent());
                    });
            }
        }
        event.Skip();
        };

    // 绑定回车和失去焦点事件
    delayCtrl->Bind(wxEVT_TEXT_ENTER, syncDelay);
    delayCtrl->Bind(wxEVT_KILL_FOCUS, syncDelay);

    // 操作符修改绑定 (exp.is_blocking)
    opChoice->Bind(wxEVT_CHOICE, [this, &exp](wxCommandEvent& e) {
        exp.is_blocking = (e.GetSelection() == 0); // 0是 "=", 1是 "<="
        this->CallAfter([this]() {
            wxPostEvent(this, wxCommandEvent());
            });
        });

    // RHS 修改绑定 (exp.right_hand_side)
    // 复用你提供的 syncFunc 逻辑
    rhsCtrl->SetClientData(&exp.nb_or_b_expression);
    auto syncRHS = [this](wxEvent& event) {
        wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
        if (ctrl && ctrl->GetClientData()) {
            auto* dataPtr = static_cast<std::string*>(ctrl->GetClientData());
            *dataPtr = ctrl->GetValue().ToStdString();

            this->CallAfter([this]() {
                wxPostEvent(this, wxCommandEvent());
                });
        }
        event.Skip();
        };
    rhsCtrl->Bind(wxEVT_TEXT_ENTER, syncRHS);
    rhsCtrl->Bind(wxEVT_KILL_FOCUS, syncRHS);



    groupSizer->Add(row, 0, wxEXPAND | wxBOTTOM, 8);
}

void SFNPropertyPanel::Add_BN_OR_B_Ports(wxSizer* groupSizer, AlwaysNode* an, std::vector<Port>& in_ports, std::vector<Port>& out_ports, int exp_id) {
    NB_OR_B_Expression& exp = an->nb_or_b_expressions[exp_id];
    Add_BN_OR_B_Port(groupSizer, out_ports[exp.out_port_id]);
    for (int id : exp.in_port_ids) {
        Add_BN_OR_B_Port(groupSizer, in_ports[id]);
    }
    // --- 底部按钮组 ---
    wxBoxSizer* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton* addBtn = new wxButton(this, wxID_ANY, "+ Add Port", wxDefaultPosition, wxDefaultSize);
    addBtn->SetForegroundColour(wxColour(0, 120, 215)); // 可选：设置为蓝色，增加视觉识别度

    addBtn->Bind(wxEVT_BUTTON, [this, an, exp_id](wxCommandEvent&) {
        an->AddExpressionsPort(exp_id);

        this->LoadNode(m_node);
        });

    wxButton* delBtn = new wxButton(this, wxID_ANY, "- Delete Port", wxDefaultPosition, wxDefaultSize);
    delBtn->SetForegroundColour(wxColour(200, 0, 0)); // 可选：设置为蓝色，增加视觉识别度

    // 4. 绑定点击事件
    delBtn->Bind(wxEVT_BUTTON, [this, an, &exp](wxCommandEvent&) {


        if (!exp.in_port_ids.empty()) {
            int pop_id = exp.in_port_ids.back();
            exp.in_port_ids.pop_back();
            an->DelExpressionsPort(pop_id);
            
        }
        else {
            wxLogWarning("Must keep at least output port.");
        }

        this->LoadNode(m_node);
        });

    btnSizer->Add(addBtn, 0, wxALL, 5);
    btnSizer->Add(delBtn, 0, wxALL, 5);

    // 再次提醒：m_mainSizer 是垂直的，这里只能用水平居中标志！
    groupSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 15);
}

void SFNPropertyPanel::Add_BN_OR_B_Port(wxSizer* groupSizer, Port& p) {
    wxBoxSizer* itemWrapper = new wxBoxSizer(wxVERTICAL);

    // --- 2. 核心：创建水平行 (让所有控件排成一排) ---
    wxBoxSizer* contentRow = new wxBoxSizer(wxHORIZONTAL);

    // --- 方向 & 名称容器 ---
    wxTextCtrl* dirC = new wxTextCtrl(this, wxID_ANY, SigFlowTree::ToString(p.direction));
    dirC->SetEditable(false);
    dirC->SetBackgroundColour(this->GetBackgroundColour());

    wxTextCtrl* editName = new wxTextCtrl(this, wxID_ANY, p.identifier);
    editName->SetEditable(false);
    editName->SetBackgroundColour(this->GetBackgroundColour());

    // --- 标签 & 输入框 ---
    wxStaticText* colon = new wxStaticText(this, wxID_ANY, "connected by");
    colon->SetForegroundColour(wxColour(120, 120, 120));

    wxTextCtrl* editConn = new wxTextCtrl(this, wxID_ANY, p.conn, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    editConn->SetHint("Connected Signal");
    editConn->SetBackgroundColour(wxColour(240, 248, 255));
    editConn->SetClientData(&p.conn);

    auto syncFunc = [this](wxEvent& event) {
        wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
        if (!ctrl) {
            event.Skip();
            return;
        }

        auto* dataPtr = static_cast<std::string*>(ctrl->GetClientData());

        if (dataPtr) {
            *dataPtr = ctrl->GetValue().ToStdString();
        }

        // 不在当前事件栈中触发 LoadNode 或 UI 重建
        CallAfter([this]() {
            wxCommandEvent evt;
            wxPostEvent(this, evt);
            });

        if (event.GetEventType() == wxEVT_TEXT_ENTER) {
            this->GetParent()->SetFocus();
        }

        event.Skip();
        };

    editConn->Bind(wxEVT_TEXT_ENTER, syncFunc);
    editConn->Bind(wxEVT_KILL_FOCUS, syncFunc);

    // --- 3. 核心布局修正：水平行里使用垂直居中是合法的 ---
    contentRow->Add(dirC, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
    contentRow->Add(editName, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
    contentRow->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
    contentRow->Add(editConn, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    // --- 4. 组装：把水平行和线加进包装器 ---
    itemWrapper->Add(contentRow, 0, wxEXPAND | wxTOP | wxBOTTOM, 5);
    itemWrapper->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

    // --- 5. 把包装器加进主面板 ---
    groupSizer->Add(itemWrapper, 0, wxEXPAND);

}

void SFNPropertyPanel::Add_BN_OR_B_Expressions(AlwaysNode* an) {
    // --- 1. 遍历并绘制现有的表达式组 ---
    for (int i = 0; i < (int)an->nb_or_b_expressions.size(); i++) {
        wxBoxSizer* groupWrapper = new wxBoxSizer(wxVERTICAL);
        NB_OR_B_Expression& exp = an->nb_or_b_expressions[i];

        Add_BN_OR_B_Expression(groupWrapper, an->out_ports, exp);
        Add_BN_OR_B_Ports(groupWrapper, an, an->in_ports, an->out_ports, i);

        // 组内分割线
        if (i < (int)an->nb_or_b_expressions.size() - 1) {
            groupWrapper->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxTOP | wxBOTTOM, 10);
        }

        m_mainSizer->Add(groupWrapper, 0, wxEXPAND | wxALL, 10);
    }

    // --- 2. 添加底部全局管理按钮组 (Add/Del Expression) ---
    m_mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxTOP | wxBOTTOM, 5);

    wxBoxSizer* globalBtnSizer = new wxBoxSizer(wxHORIZONTAL);

    wxButton* addExpBtn = new wxButton(this, wxID_ANY, "Add Expression", wxDefaultPosition, wxDefaultSize);
    wxButton* delExpBtn = new wxButton(this, wxID_ANY, "Delete Last Expression", wxDefaultPosition, wxDefaultSize);

    // 样式美化
    addExpBtn->SetBackgroundColour(wxColour(230, 255, 230)); // 淡绿色
    delExpBtn->SetBackgroundColour(wxColour(255, 230, 230)); // 淡红色

    globalBtnSizer->Add(addExpBtn, 1, wxEXPAND | wxALL, 5);
    globalBtnSizer->Add(delExpBtn, 1, wxEXPAND | wxALL, 5);

    m_mainSizer->Add(globalBtnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // --- 3. 绑定事件 ---
    addExpBtn->Bind(wxEVT_BUTTON, [this, an](wxCommandEvent&) {
        an->AddEmptyExpression();
        this->LoadNode(m_node); // 重新触发全量绘制
        });

    delExpBtn->Bind(wxEVT_BUTTON, [this, an](wxCommandEvent&) {
        if (!an->nb_or_b_expressions.empty()) {
            an->DelLastExpression();
            this->LoadNode(m_node);
        }
        });
}



