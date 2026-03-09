#include "SFNPropertyPanel.h"
#include "PropertyPanelBuilder.h"
#include <wx/statline.h>

wxDEFINE_EVENT(EVT_SFTREE_CHANGED, wxCommandEvent);

// 辅助函数：创建带前景色的添加按钮并绑定事件
static wxButton* CreateAddButton(wxWindow* parent, const wxString& label,
    std::function<void()> handler) {
    auto btn = PropertyPanelBuilder::CreateButton(parent, label, wxColour(0, 120, 215));
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
                m_tree->ReIdentifier(tn, ctrl->GetValue().ToStdString());
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
                m_tree->ReIdentifier(sn, ctrl->GetValue().ToStdString());
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
                    m_tree->ReIdentifier(sn, ctrl->GetValue().ToStdString());
                    if (event.GetEventType() == wxEVT_TEXT_ENTER)
                        this->GetParent()->SetFocus();
                    event.Skip();
                    };
                idCtrl->Bind(wxEVT_TEXT_ENTER, syncId);
                idCtrl->Bind(wxEVT_KILL_FOCUS, syncId);
            }

            // 输入端口
            for (auto& p : sn->in_ports) {
                m_mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
                m_mainSizer->Add(CreateSecondPortRowSizer(sn, p.identifier, p.direction, p.conn), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            }
            // 输出端口
            for (auto& p : sn->out_ports) {
                m_mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
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
                    m_tree->ReIdentifier(sn, ctrl->GetValue().ToStdString());
                    if (event.GetEventType() == wxEVT_TEXT_ENTER)
                        this->GetParent()->SetFocus();
                    event.Skip();
                    };
                idCtrl->Bind(wxEVT_TEXT_ENTER, syncId);
                idCtrl->Bind(wxEVT_KILL_FOCUS, syncId);
            }

            for (auto& p : sn->in_ports) {
                m_mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
                m_mainSizer->Add(CreateSecondPortRowSizer(sn, p.identifier, p.direction, p.conn), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            }
            for (auto& p : sn->out_ports) {
                m_mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
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
                    *dataPtr = ctrl->GetValue().ToStdString();
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
    std::vector<Port>& ports = (dir == PortDirection::In) ? tn->GetInPorts() : tn->GetOutPorts();
    wxString dirStr = SigFlowTree::ToString(dir);

    for (Port& p : ports) {
        wxTextCtrl* nameCtrl = nullptr;
        wxButton* delBtn = nullptr;
        auto wrapper = PropertyPanelBuilder::CreateTopPortRow(this, dirStr, p.identifier, &nameCtrl, &delBtn);
        outerSizer->Add(wrapper, 0, wxEXPAND);

        // 绑定端口重命名
        if (nameCtrl) {
            wxString oldId = p.identifier;
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
            delBtn->Bind(wxEVT_BUTTON, [this, tn, id = p.identifier](wxCommandEvent&) {
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
    wxTextCtrl* connCtrl = nullptr;
    auto wrapper = PropertyPanelBuilder::CreateSecondPortRow(this, SigFlowTree::ToString(dir),
        name, wxString::FromUTF8(conn), &connCtrl);
    if (connCtrl) {
        wxString portName = name;
        auto syncConn = [this, sn, portName](wxEvent& event) {
            wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
            if (!ctrl) { event.Skip(); return; }
            m_tree->PortConn(sn, portName, ctrl->GetValue().ToStdString());
            CallAfter([this]() { wxPostEvent(this, wxCommandEvent()); });
            if (event.GetEventType() == wxEVT_TEXT_ENTER)
                this->GetParent()->SetFocus();
            event.Skip();
            };
        connCtrl->Bind(wxEVT_TEXT_ENTER, syncConn);
        connCtrl->Bind(wxEVT_KILL_FOCUS, syncConn);
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
    auto delBtn = PropertyPanelBuilder::CreateButton(this, "- Delete Port", wxColour(200, 0, 0));
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

        Add_BN_OR_B_Expression(groupWrapper, an, stmt);
        Add_BN_OR_B_Ports(groupWrapper, an, stmt, static_cast<int>(i));

        // ---- 新增：每个表达式独立的删除按钮 ----
        auto deleteExpBtn = PropertyPanelBuilder::CreateButton(this, "Delete Expression", wxColour(200, 0, 0));
        deleteExpBtn->Bind(wxEVT_BUTTON, [this, an, i](wxCommandEvent&) {
            if (an->getStatementCount() > i) {
                // 先获取要删除语句的输出端口名
                auto* targetStmt = dynamic_cast<const AlwaysStatement*>(an->getStatement(i));
                if (targetStmt) {
                    std::string outName = targetStmt->out_port_name;
                    // 删除语句
                    an->removeStatement(i);
                    // 同步删除输出端口
                    auto& outPorts = an->out_ports;
                    outPorts.erase(std::remove_if(outPorts.begin(), outPorts.end(),
                        [&outName](const Port& p) { return p.identifier == outName; }),
                        outPorts.end());
                    // 清理无用的输入端口
                    an->CleanUnusedInPorts();
                }
                LoadNode(m_node);
            }
            });
        groupWrapper->Add(deleteExpBtn, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 5);
        // ---------------------------------------

        if (i < an->getStatementCount() - 1)
            groupWrapper->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxTOP | wxBOTTOM, 10);

        mainSizer->Add(groupWrapper, 0, wxEXPAND | wxALL, 10);
    }

    // 全局按钮（添加表达式、删除最后一个表达式）保持不变
    mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxTOP | wxBOTTOM, 5);
    auto globalBtnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto addExpBtn = PropertyPanelBuilder::CreateButton(this, "Add Expression");
    addExpBtn->SetBackgroundColour(wxColour(230, 255, 230));
    auto delLastExpBtn = PropertyPanelBuilder::CreateButton(this, "Delete Last Expression");
    delLastExpBtn->SetBackgroundColour(wxColour(255, 230, 230));
    globalBtnSizer->Add(addExpBtn, 1, wxEXPAND | wxALL, 5);
    globalBtnSizer->Add(delLastExpBtn, 1, wxEXPAND | wxALL, 5);
    mainSizer->Add(globalBtnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    addExpBtn->Bind(wxEVT_BUTTON, [this, an](wxCommandEvent&) {
        an->AddEmptyExpression();
        LoadNode(m_node);
        });
    delLastExpBtn->Bind(wxEVT_BUTTON, [this, an](wxCommandEvent&) {
        an->DelLastExpression();  // 此函数已清理输出端口
        an->CleanUnusedInPorts(); // 清理无用的输入端口
        LoadNode(m_node);
        });

    return mainSizer;
}

void SFNPropertyPanel::Add_BN_OR_B_Expression(wxSizer* groupSizer, AlwaysNode* an, AlwaysStatement* stmt) {
    wxString outName = wxString::FromUTF8(stmt->out_port_name);

    wxTextCtrl* delayCtrl = nullptr;
    wxChoice* opChoice = nullptr;
    wxTextCtrl* rhsCtrl = nullptr;

    auto rowSizer = PropertyPanelBuilder::CreateNBOrBExpressionRow(this, stmt->delay, outName,
        stmt->is_blocking,
        wxString::FromUTF8(stmt->nb_or_b_expression),
        &delayCtrl, &opChoice, &rhsCtrl);
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
                *dataPtr = ctrl->GetValue().ToStdString();
                CallAfter([this]() { wxPostEvent(this, wxCommandEvent()); });
            }
            event.Skip();
            };
        rhsCtrl->Bind(wxEVT_TEXT_ENTER, syncRHS);
        rhsCtrl->Bind(wxEVT_KILL_FOCUS, syncRHS);
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

    // 底部按钮组：添加/删除端口
    auto btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto addBtn = CreateAddButton(this, "+ Add Port", [this, an, stmt]() {
        an->AddPortToExpression(stmt);
        LoadNode(m_node);
        });
    auto delBtn = PropertyPanelBuilder::CreateButton(this, "- Delete Port", wxColour(200, 0, 0));
    delBtn->Bind(wxEVT_BUTTON, [this, an, stmt](wxCommandEvent&) {
        if (!stmt->in_port_names.empty()) {
            std::string lastPortName = stmt->in_port_names.back();
            int index = -1;
            for (size_t i = 0; i < an->in_ports.size(); ++i) {
                if (an->in_ports[i].identifier == lastPortName) {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index != -1) {
                an->DeletePort(index);
                LoadNode(m_node);
            }
        }
        else {
            wxLogWarning("Must keep at least one input port.");
        }
        });
    btnSizer->Add(addBtn, 0, wxALL, 5);
    btnSizer->Add(delBtn, 0, wxALL, 5);
    groupSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 15);
}

void SFNPropertyPanel::Add_BN_OR_B_Port(wxSizer* groupSizer, AlwaysNode* an, Port& p) {
    wxTextCtrl* connCtrl = nullptr;
    auto wrapper = PropertyPanelBuilder::CreateSecondPortRow(this, SigFlowTree::ToString(p.direction),
        p.identifier, wxString::FromUTF8(p.conn),
        &connCtrl);
    if (connCtrl) {
        wxString portName = p.identifier;
        auto syncConn = [this, an, portName](wxEvent& event) {
            wxTextCtrl* ctrl = wxDynamicCast(event.GetEventObject(), wxTextCtrl);
            if (!ctrl) { event.Skip(); return; }
            m_tree->PortConn(an, portName, ctrl->GetValue().ToStdString());
            CallAfter([this]() { wxPostEvent(this, wxCommandEvent()); });
            if (event.GetEventType() == wxEVT_TEXT_ENTER)
                this->GetParent()->SetFocus();
            event.Skip();
            };
        connCtrl->Bind(wxEVT_TEXT_ENTER, syncConn);
        connCtrl->Bind(wxEVT_KILL_FOCUS, syncConn);
    }
    groupSizer->Add(wrapper, 0, wxEXPAND);
}
