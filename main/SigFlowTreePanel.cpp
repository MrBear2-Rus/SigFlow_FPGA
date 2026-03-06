#include "SigFlowTreePanel.h"
#include "PropertyPanelBuilder.h"
#include <wx/statline.h>

SigFlowTreePanel::SigFlowTreePanel(wxWindow* parent,SigFlowTree* sfTree)
    : wxPanel(parent) {
    this->sfTree = sfTree;
    tree = new wxTreeCtrl(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxTR_DEFAULT_STYLE | wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT);


    addBtn = new wxButton(this, wxID_ANY, "Add Node");
    delBtn = new wxButton(this, wxID_ANY, "Delete Node");

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->Add(addBtn, 0, wxRIGHT, 6);
    btnSizer->Add(delBtn, 0);

    auto* rootSizer = new wxBoxSizer(wxVERTICAL);
    rootSizer->Add(btnSizer, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    rootSizer->Add(tree, 1, wxEXPAND | wxALL, 6);

    SetSizer(rootSizer);

    tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &SigFlowTreePanel::OnItemActivated, this);
    addBtn->Bind(wxEVT_BUTTON, &SigFlowTreePanel::OnAdd, this);
    delBtn->Bind(wxEVT_BUTTON, &SigFlowTreePanel::OnDelete, this);

    Fresh();
}

void SigFlowTreePanel::BuildBranch(wxTreeItemId uiParent, SigTreeNode* logicParent) {
    for (auto* child : logicParent->GetChildren()) {
        wxTreeItemId uiChild = tree->AppendItem(
            uiParent,
            child->GetName(), // 自动根据类型返回正确的名字
            -1, -1,
            new SigTreeItemData(child)
        );
        BuildBranch(uiChild, child);
        if (fn) {
            if (fn->GetName() == child->GetName()) {
                tree->Expand(uiChild);
                tree->EnsureVisible(uiChild);
            }
            else {
                for (auto* cld : fn->GetChildren()) {
                    if (cld->GetName() == child->GetName()) tree->Expand(uiChild);
                }
            }
        }
        else {
            tree->Expand(uiChild);
        }
        
    }
}

void SigFlowTreePanel::OnItemActivated(wxTreeEvent& event) {
    wxTreeItemId id = event.GetItem();
    if (!id.IsOk()) return;

    // 1. 提取绑定的数据
    SigTreeItemData* data = static_cast<SigTreeItemData*>(tree->GetItemData(id));
    if (data && data->node) {
        // 2. 包装并向上传递
        wxCommandEvent evt(EVT_SFTREE_NODE_ACTIVATED, GetId());

        // 将指针存入 ClientData (注意：CommandEvent 只能存 void*)
        evt.SetClientData(static_cast<void*>(data->node));

        // 向上传播
        GetParent()->ProcessWindowEvent(evt);
        
    }
}

void SigFlowTreePanel::Fresh() {
    if (!tree) return;



    tree->Freeze(); // 防止频繁重绘闪烁
    tree->DeleteAllItems();

    if (!sfTree || !sfTree->root) {
        tree->Thaw();
        return;
    }

    SigTreeNode* logicRoot = sfTree->root;
    if (!logicRoot) return;

    wxString rootLabel;
    if (sfTree->root->type == SigTreeNodeType::Project) {
        auto proj = static_cast<ProjectNode*>(sfTree->root);
        // 提取文件名作为显示名称，或者直接显示路径
        rootLabel = wxString::FromUTF8(proj->projectPath);
    }
    else {
        rootLabel = "Unknown Project";
    }

    wxTreeItemId uiRoot = tree->AddRoot(
        rootLabel,
        -1, -1,
        new SigTreeItemData(sfTree->root)
    );

    this->BuildBranch(uiRoot, logicRoot);
    tree->Expand(uiRoot);
    tree->Thaw();
}

void SigFlowTreePanel::OnAdd(wxCommandEvent&) {
    wxTreeItemId sel = tree->GetSelection();
    if (!sel.IsOk()) return;

    auto* data = static_cast<SigTreeItemData*>(tree->GetItemData(sel));
    if (!data || !data->node) return;

    SigTreeNode* parent = data->node;

    wxMenu menu;

    wxMenuItem* addModule = menu.Append(1001, "Add Module");
    wxMenuItem* addWire = menu.Append(1002, "Add Wire");
    wxMenuItem* addReg = menu.Append(1003, "Add Reg");
    wxMenuItem* addInst = menu.Append(1004, "Add Module Instance");
    wxMenuItem* addGate = menu.Append(1005, "Add Gate");
    wxMenuItem* addAssign = menu.Append(1006, "Add Assignment");
    wxMenuItem* addAlways = menu.Append(1007, "Add Always Block");

    // -------- 根据上下文启用 / 禁用 --------
    bool isFile = parent->type == SigTreeNodeType::File;
    bool isTop = parent->type == SigTreeNodeType::Top;
    bool isSec = parent->type == SigTreeNodeType::Second;
    addModule->Enable(isFile);

    addWire->Enable(isTop);
    addReg->Enable(isTop );
    addInst->Enable(isTop );
    addGate->Enable(isTop );
    addAssign->Enable(isTop );
    addAlways->Enable(isTop);

    menu.Bind(wxEVT_MENU, &SigFlowTreePanel::OnAddMenu, this);
    addBtn->PopupMenu(&menu, 0, addBtn->GetSize().y);
}


void SigFlowTreePanel::OnAddMenu(wxCommandEvent& e) {
    wxTreeItemId sel = tree->GetSelection();
    if (!sel.IsOk()) return;

    auto* data = static_cast<SigTreeItemData*>(tree->GetItemData(sel));
    if (!data || !data->node) return;

    SigTreeNode* parent = data->node;

    std::unique_ptr<SigTreeNode> newNode;

    switch (e.GetId()) {
    case 1001: // Add Module
        newNode.reset(ShowCreateTopDialog(TopNodeType::Module, parent));
        break;
    case 1002: // Add Wire
        newNode.reset(ShowCreateSignalDialog(SignalType::Wire, parent));
        break;
    case 1003: // Add Reg
        newNode.reset(ShowCreateSignalDialog(SignalType::Reg, parent));
        break;
    case 1004: // Add Module Instance
        newNode.reset(ShowCreateSecondDialog(SecondNodeType::ModuleInstance, parent));
        break;
    case 1005: // Add Gate
        newNode.reset(ShowCreateSecondDialog(SecondNodeType::GateInstance, parent));
        break;
    case 1006: // Add Assignment
        newNode.reset(ShowCreateSecondDialog(SecondNodeType::ContinuousAssign, parent));
        break;
    case 1007: // Add Always
        newNode.reset(ShowCreateSecondDialog(SecondNodeType::Always, parent));
        break;
    default:
        return;
    }

    if (!newNode) return; // 用户点了 Cancel

    sfTree->AddChild(parent, newNode.get());

    Fresh();
}


void SigFlowTreePanel::OnDelete(wxCommandEvent&) {
    wxTreeItemId sel = tree->GetSelection();
    if (!sel.IsOk()) return;

    auto* data = static_cast<SigTreeItemData*>(tree->GetItemData(sel));
    if (!data || !data->node) return;

    SigTreeNode* node = data->node;
    if (!node->GetParent()) return; // 不允许删 root

    // ---- 修改模型 ----
    sfTree->RemoveChild(node->GetParent(), node);

    // ---- 同步刷新 ----
    Fresh();

}

TopNode* SigFlowTreePanel::ShowCreateTopDialog(TopNodeType type, SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Module", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    // 临时数据
    std::vector<Port> in_ports, out_ports;

    // 标识符行
    wxTextCtrl* idCtrl = nullptr;
    auto* topSizer = new wxBoxSizer(wxVERTICAL);
    topSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(&dlg, "Identifier:", "module_name", &idCtrl),
        0, wxEXPAND | wxALL, 10);

    // 滚动窗口用于端口列表
    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(400, 250));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // 添加端口按钮
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* addInBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add In", wxColour(0, 120, 215));
    auto* addOutBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add Out", wxColour(0, 120, 215));
    btnSizer->Add(addInBtn, 0, wxRIGHT, 10);
    btnSizer->Add(addOutBtn, 0);
    topSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 10);

    // 标准按钮
    topSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(topSizer);

    // 重建函数
    std::function<void()> rebuild = [&]() {
        portsSizer->Clear(true);
        // 输入端口
        for (size_t i = 0; i < in_ports.size(); ++i) {
            wxTextCtrl* nameCtrl = nullptr;
            wxButton* delBtn = nullptr;
            auto* rowWrapper = PropertyPanelBuilder::CreateTopPortRow(scrolled, "input",
                wxString::FromUTF8(in_ports[i].identifier), &nameCtrl, &delBtn);
            if (nameCtrl) {
                nameCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent&) {
                    in_ports[i].identifier = nameCtrl->GetValue().ToStdString();
                    });
            }
            if (delBtn) {
                delBtn->Bind(wxEVT_BUTTON, [&, i](wxCommandEvent&) {
                    in_ports.erase(in_ports.begin() + i);
                    rebuild();
                    });
            }
            portsSizer->Add(rowWrapper, 0, wxEXPAND);
        }
        // 输出端口
        for (size_t i = 0; i < out_ports.size(); ++i) {
            wxTextCtrl* nameCtrl = nullptr;
            wxButton* delBtn = nullptr;
            auto* rowWrapper = PropertyPanelBuilder::CreateTopPortRow(scrolled, "output",
                wxString::FromUTF8(out_ports[i].identifier), &nameCtrl, &delBtn);
            if (nameCtrl) {
                nameCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent&) {
                    out_ports[i].identifier = nameCtrl->GetValue().ToStdString();
                    });
            }
            if (delBtn) {
                delBtn->Bind(wxEVT_BUTTON, [&, i](wxCommandEvent&) {
                    out_ports.erase(out_ports.begin() + i);
                    rebuild();
                    });
            }
            portsSizer->Add(rowWrapper, 0, wxEXPAND);
        }
        scrolled->Layout();
        scrolled->FitInside();
        };

    // 添加端口按钮事件
    addInBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        Port p;
        p.identifier = "in" + std::to_string(in_ports.size() + 1);
        p.direction = PortDirection::In;
        in_ports.push_back(p);
        rebuild();
        });
    addOutBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        Port p;
        p.identifier = "out" + std::to_string(out_ports.size() + 1);
        p.direction = PortDirection::Out;
        out_ports.push_back(p);
        rebuild();
        });

    // 初始添加一个示例端口（可选）
    if (in_ports.empty() && out_ports.empty()) {
        in_ports.push_back({ "in1", PortDirection::In });
        out_ports.push_back({ "out1", PortDirection::Out });
        rebuild();
    }
    else {
        rebuild();
    }

    if (dlg.ShowModal() != wxID_OK) return nullptr;

    wxString id = idCtrl->GetValue();
    if (id.empty()) return nullptr;

    auto* node = new TopNode(id.ToStdString(), type);
    node->UpdateInPorts(in_ports);
    node->UpdateOutPorts(out_ports);
    return node;
}

SignalNode* SigFlowTreePanel::ShowCreateSignalDialog(SignalType type, SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Signal");
    wxTextCtrl* idCtrl = nullptr;
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(PropertyPanelBuilder::CreateIdentifierRow(&dlg, "Identifier:", "", &idCtrl),
        0, wxEXPAND | wxALL, 10);
    sizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(sizer);

    if (dlg.ShowModal() != wxID_OK) return nullptr;
    wxString id = idCtrl->GetValue();
    if (id.empty()) return nullptr;
    return new SignalNode(id.ToStdString(), type);
}

SecondNode* SigFlowTreePanel::ShowCreateSecondDialog(SecondNodeType type, SigTreeNode* parent) {
    switch (type) {
    case SecondNodeType::ModuleInstance:{
        return CreateModuleInstDialog(parent);
    }
    case SecondNodeType::GateInstance: {
        return CreateGateInstDialog(parent);
    }
    case SecondNodeType::ContinuousAssign:{
        return CreateContiniousAssignDialog(parent);
     }
    }
}

SecondNode* SigFlowTreePanel::CreateModuleInstDialog(SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Instantiate Module", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    // 获取所有可实例化的模块定义（排除当前模块自身）
    wxArrayString defNames;
    std::vector<TopNode*> defPointers;
    for (auto const& [name, defPtr] : sfTree->DefinitionTable) {
        if (defPtr->topType == TopNodeType::Module) {
            // 如果是当前模块内部，通常不能实例化自身，但可根据需求调整
            TopNode* tn = dynamic_cast<TopNode*>(parent);
            if (!tn || name != tn->identifier) {
                defNames.Add(name);
                defPointers.push_back(defPtr);
            }
        }
    }
    if (defNames.IsEmpty()) {
        wxMessageBox("No Module Definitions found!", "Error", wxOK | wxICON_ERROR);
        return nullptr;
    }

    // 临时数据
    std::vector<Port> in_ports, out_ports;

    // 顶部标识符和定义选择
    wxTextCtrl* idCtrl = nullptr;
    wxChoice* defChoice = nullptr;
    auto* topSizer = new wxBoxSizer(wxVERTICAL);
    auto* gridSizer = new wxFlexGridSizer(2, 8, 8);
    gridSizer->AddGrowableCol(1);
    gridSizer->Add(new wxStaticText(&dlg, wxID_ANY, "Identifier:"), 0, wxALIGN_CENTER_VERTICAL);
    gridSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(&dlg, "", "u_inst_0", &idCtrl), 1, wxEXPAND);
    gridSizer->Add(new wxStaticText(&dlg, wxID_ANY, "Definition:"), 0, wxALIGN_CENTER_VERTICAL);
    gridSizer->Add(PropertyPanelBuilder::CreateChoiceRow(&dlg, "", defNames, 0, &defChoice), 1, wxEXPAND);
    topSizer->Add(gridSizer, 0, wxEXPAND | wxALL, 10);

    // 滚动窗口用于端口列表
    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(500, 300));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
    topSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(topSizer);

    // 重建函数
    std::function<void(int)> rebuild = [&](int sel) {
        portsSizer->Clear(true);
        in_ports.clear();
        out_ports.clear();
        TopNode* def = defPointers[sel];

        // 从定义复制端口，conn 初始为空
        for (const auto& p : def->GetInPorts()) {
            in_ports.push_back({ p.identifier, p.direction, "" });
        }
        for (const auto& p : def->GetOutPorts()) {
            out_ports.push_back({ p.identifier, p.direction, "" });
        }

        // 创建输入端口行
        for (size_t i = 0; i < in_ports.size(); ++i) {
            wxTextCtrl* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(in_ports[i].direction),
                wxString::FromUTF8(in_ports[i].identifier),
                wxString::FromUTF8(in_ports[i].conn),
                &connCtrl);
            if (connCtrl) {
                connCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent& e) {
                    in_ports[i].conn = e.GetString().ToStdString();
                    });
            }
            portsSizer->Add(wrapper, 0, wxEXPAND);
        }
        // 创建输出端口行
        for (size_t i = 0; i < out_ports.size(); ++i) {
            wxTextCtrl* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(out_ports[i].direction),
                wxString::FromUTF8(out_ports[i].identifier),
                wxString::FromUTF8(out_ports[i].conn),
                &connCtrl);
            if (connCtrl) {
                connCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent& e) {
                    out_ports[i].conn = e.GetString().ToStdString();
                    });
            }
            portsSizer->Add(wrapper, 0, wxEXPAND);
        }
        scrolled->Layout();
        scrolled->FitInside();
        };

    defChoice->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) {
        rebuild(defChoice->GetSelection());
        });
    rebuild(0);

    if (dlg.ShowModal() != wxID_OK) return nullptr;

    wxString id = idCtrl->GetValue();
    if (id.empty()) return nullptr;
    wxString defName = defChoice->GetStringSelection();

    auto* node = new ModuleInstNode(id.ToStdString(), defName.ToStdString());
    node->in_ports = in_ports;
    node->out_ports = out_ports;
    // 后续在 AddChild 时会自动链接定义
    return node;
}

SecondNode* SigFlowTreePanel::CreateGateInstDialog(SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Gate Instance", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    // 临时端口数据
    std::vector<Port> ports;  // 包含所有端口，方向由定义决定

    // 顶部标识符和门类型
    wxTextCtrl* idCtrl = nullptr;
    wxChoice* gateChoice = nullptr;
    auto* topSizer = new wxBoxSizer(wxVERTICAL);
    auto* gridSizer = new wxFlexGridSizer(2, 8, 8);
    gridSizer->AddGrowableCol(1);
    gridSizer->Add(new wxStaticText(&dlg, wxID_ANY, "Identifier:"), 0, wxALIGN_CENTER_VERTICAL);
    gridSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(&dlg, "", "g0", &idCtrl), 1, wxEXPAND);
    gridSizer->Add(new wxStaticText(&dlg, wxID_ANY, "Gate Type:"), 0, wxALIGN_CENTER_VERTICAL);
    wxArrayString gateTypes;
    gateTypes.Add("and"); gateTypes.Add("nand"); gateTypes.Add("or"); gateTypes.Add("nor");
    gateTypes.Add("xor"); gateTypes.Add("xnor"); gateTypes.Add("buf"); gateTypes.Add("not");
    gridSizer->Add(PropertyPanelBuilder::CreateChoiceRow(&dlg, "", gateTypes, 0, &gateChoice), 1, wxEXPAND);
    topSizer->Add(gridSizer, 0, wxEXPAND | wxALL, 10);

    // 滚动窗口
    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(500, 250));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
    topSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(topSizer);

    // 根据门类型生成端口定义
    auto getPortDefs = [](const wxString& type) -> std::vector<Port> {
        std::vector<Port> defs;
        if (type == "buf" || type == "not") {
            defs.push_back({ "Out1", PortDirection::Out, "" });
            defs.push_back({ "Out2", PortDirection::Out, "" });
            defs.push_back({ "In1", PortDirection::In, "" });
        }
        else {
            defs.push_back({ "Out1", PortDirection::Out, "" });
            defs.push_back({ "In1", PortDirection::In, "" });
            defs.push_back({ "In2", PortDirection::In, "" });
        }
        return defs;
        };

    // 重建函数
    std::function<void()> rebuild = [&]() {
        portsSizer->Clear(true);
        ports = getPortDefs(gateChoice->GetStringSelection());
        for (size_t i = 0; i < ports.size(); ++i) {
            wxTextCtrl* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(ports[i].direction),
                wxString::FromUTF8(ports[i].identifier),
                wxString::FromUTF8(ports[i].conn),
                &connCtrl);
            if (connCtrl) {
                connCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent& e) {
                    ports[i].conn = e.GetString().ToStdString();
                    });
            }
            portsSizer->Add(wrapper, 0, wxEXPAND);
        }
        scrolled->Layout();
        scrolled->FitInside();
        };

    gateChoice->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { rebuild(); });
    rebuild();

    if (dlg.ShowModal() != wxID_OK) return nullptr;

    wxString id = idCtrl->GetValue();
    if (id.empty()) return nullptr;
    GateType gt = SigFlowTree::GateTypeFromString(gateChoice->GetStringSelection().ToStdString());
    auto* node = new GateInstNode(id.ToStdString(), gt);
    // 根据方向分配 in/out ports
    for (const auto& p : ports) {
        if (p.direction == PortDirection::In)
            node->in_ports.push_back(p);
        else
            node->out_ports.push_back(p);
    }
    return node;
}

SecondNode* SigFlowTreePanel::CreateContiniousAssignDialog(SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Continuous Assignment", wxDefaultPosition, wxSize(500, 600),
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    // 临时数据
    std::string expr;
    std::vector<Port> in_ports;
    std::vector<Port> out_ports = { {"Out1", PortDirection::Out, ""} }; // 默认一个输出

    auto* topSizer = new wxBoxSizer(wxVERTICAL);

    // 表达式行
    wxTextCtrl* exprCtrl = nullptr;
    topSizer->Add(PropertyPanelBuilder::CreateEditableTextRow(&dlg, "Expression:", expr, &exprCtrl),
        0, wxEXPAND | wxALL, 10);
    if (exprCtrl) {
        exprCtrl->Bind(wxEVT_TEXT, [&](wxCommandEvent& e) { expr = e.GetString().ToStdString(); });
    }

    // 滚动窗口用于端口
    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(-1, 300));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // 添加/删除端口按钮
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* addBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add Port", wxColour(0, 120, 215));
    auto* delBtn = PropertyPanelBuilder::CreateButton(&dlg, "- Delete Port", wxColour(200, 0, 0));
    btnSizer->Add(addBtn, 0, wxRIGHT, 10);
    btnSizer->Add(delBtn, 0);
    topSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 10);
    topSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(topSizer);

    // 重建函数
    std::function<void()> rebuild = [&]() {
        portsSizer->Clear(true);
        // 输出端口
        for (size_t i = 0; i < out_ports.size(); ++i) {
            wxTextCtrl* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(out_ports[i].direction),
                wxString::FromUTF8(out_ports[i].identifier),
                wxString::FromUTF8(out_ports[i].conn),
                &connCtrl);
            if (connCtrl) {
                connCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent& e) {
                    out_ports[i].conn = e.GetString().ToStdString();
                    });
            }
            portsSizer->Add(wrapper, 0, wxEXPAND);
        }
        // 输入端口
        for (size_t i = 0; i < in_ports.size(); ++i) {
            wxTextCtrl* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(in_ports[i].direction),
                wxString::FromUTF8(in_ports[i].identifier),
                wxString::FromUTF8(in_ports[i].conn),
                &connCtrl);
            if (connCtrl) {
                connCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent& e) {
                    in_ports[i].conn = e.GetString().ToStdString();
                    });
            }
            portsSizer->Add(wrapper, 0, wxEXPAND);
        }
        scrolled->Layout();
        scrolled->FitInside();
        };

    addBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        Port p;
        p.identifier = "In" + std::to_string(in_ports.size() + 1);
        p.direction = PortDirection::In;
        in_ports.push_back(p);
        rebuild();
        });

    delBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        if (!in_ports.empty()) {
            in_ports.pop_back();
            rebuild();
        }
        });

    rebuild(); // 初始显示

    if (dlg.ShowModal() != wxID_OK) return nullptr;

    wxString id = out_ports[0].identifier; // 或者从 exprCtrl 派生？原代码是用 out_ports[0].identifier 作为 id？实际上 ContinuousAssignNode 构造函数接受 id 和 raw_assign，这里我们用 out_ports[0].identifier 作为 id，expr 作为 raw_assign。
    auto* node = new ContinuousAssignNode(id.ToStdString(), expr);
    node->in_ports = in_ports;
    node->out_ports = out_ports;
    return node;
}
