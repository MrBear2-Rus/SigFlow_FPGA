#include "SigFlowTreePanel.h"
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
    for (auto* child : logicParent->children) {
        wxTreeItemId uiChild = tree->AppendItem(
            uiParent,
            child->GetDisplayName(), // 自动根据类型返回正确的名字
            -1, -1,
            new SigTreeItemData(child)
        );
        BuildBranch(uiChild, child);
        tree->Expand(uiChild);
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
    if (logicRoot->type == SigTreeNodeType::Project) {
        auto proj = static_cast<ProjectNode*>(logicRoot);
        // 提取文件名作为显示名称，或者直接显示路径
        rootLabel = wxString::FromUTF8(proj->projectPath);
    }
    else {
        rootLabel = "Unknown Project";
    }

    wxTreeItemId uiRoot = tree->AddRoot(
        rootLabel,
        -1, -1,
        new SigTreeItemData(logicRoot)
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
    wxCommandEvent evt(EVT_SFTREE_CHANGED);
    ProcessWindowEvent(evt);
}


void SigFlowTreePanel::OnDelete(wxCommandEvent&) {
    wxTreeItemId sel = tree->GetSelection();
    if (!sel.IsOk()) return;

    auto* data = static_cast<SigTreeItemData*>(tree->GetItemData(sel));
    if (!data || !data->node) return;

    SigTreeNode* node = data->node;
    if (!node->parent) return; // 不允许删 root

    // ---- 修改模型 ----
    sfTree->RemoveChild(node->parent, node);

    // ---- 同步刷新 ----
    Fresh();

    // ---- 通知 ----
    wxCommandEvent evt(EVT_SFTREE_CHANGED);
    ProcessWindowEvent(evt);
}

TopNode* SigFlowTreePanel::ShowCreateTopDialog(TopNodeType type, SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Module", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    auto* idCtrl = new wxTextCtrl(&dlg, wxID_ANY);

    std::vector<Port> ports;

    auto* portsSizer = new wxBoxSizer(wxVERTICAL);

    // ---- rebuild function (no capture of dlg) ----
    std::function<void()> rebuild;
    rebuild = [&]() {
        portsSizer->Clear(true);

        for (size_t i = 0; i < ports.size(); ++i) {
            Port& p = ports[i];

            auto* row = new wxBoxSizer(wxHORIZONTAL);

            wxArrayString choices;
            choices.Add("input");
            choices.Add("output");
            choices.Add("inout");
            choices.Add("ref");

            auto* dir = new wxChoice(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
            dir->SetSelection((int)p.direction);

            auto* name = new wxTextCtrl(&dlg, wxID_ANY, p.identifier);

            auto* del = new wxButton(&dlg, wxID_ANY, "x", wxDefaultPosition, wxSize(25, 25));

            dir->Bind(wxEVT_CHOICE, [&, i](wxCommandEvent& e) {
                ports[i].direction = static_cast<PortDirection>(e.GetSelection());
                });

            name->Bind(wxEVT_TEXT, [&, i](wxCommandEvent& e) {
                ports[i].identifier = e.GetString().ToStdString();
                });

            del->Bind(wxEVT_BUTTON, [&, i](wxCommandEvent&) {
                ports.erase(ports.begin() + i);
                rebuild();
                });

            row->Add(dir, 0, wxRIGHT, 6);
            row->Add(name, 1, wxEXPAND | wxRIGHT, 6);
            row->Add(del, 0);

            portsSizer->Add(row, 0, wxEXPAND | wxBOTTOM, 4);
        }

        auto* addBtn = new wxButton(&dlg, wxID_ANY, "+ Add Port");
        addBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
            Port p;
            p.identifier = "port" + std::to_string(ports.size());
            p.direction = PortDirection::In;
            ports.push_back(p);
            rebuild();
            });

        portsSizer->Add(addBtn, 0, wxTOP, 6);

        dlg.Layout();
        dlg.Fit();
        };

    rebuild();

    // ---- Layout ----
    auto* form = new wxFlexGridSizer(2, 8, 8);
    form->Add(new wxStaticText(&dlg, wxID_ANY, "Identifier:"), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(idCtrl, 1, wxEXPAND);
    form->AddGrowableCol(1);

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(form, 0, wxEXPAND | wxALL, 10);
    root->Add(new wxStaticText(&dlg, wxID_ANY, "Ports:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
    root->Add(portsSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    root->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL),
        0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    dlg.SetSizerAndFit(root);

    if (dlg.ShowModal() != wxID_OK)
        return nullptr;

    wxString id = idCtrl->GetValue();
    if (id.empty())
        return nullptr;

    auto* node = new TopNode();
    node->type = SigTreeNodeType::Top;
    node->topType = type;
    node->identifier = id.ToStdString();
    node->ports = std::move(ports);

    return node;
}

SignalNode* SigFlowTreePanel::ShowCreateSignalDialog(SignalType type, SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Signal");

    wxTextCtrl* idCtrl = new wxTextCtrl(&dlg, wxID_ANY);

    auto* form = new wxFlexGridSizer(2, 6, 6);
    form->Add(new wxStaticText(&dlg, wxID_ANY, "Identifier:"));
    form->Add(idCtrl, 1, wxEXPAND);
    form->AddGrowableCol(1);

    auto* btns = dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL);

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(form, 1, wxEXPAND | wxALL, 10);
    root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    dlg.SetSizerAndFit(root);

    if (dlg.ShowModal() != wxID_OK)
        return nullptr;

    wxString id = idCtrl->GetValue();
    if (id.empty())
        return nullptr;

    auto* node = new SignalNode();
    node->type = SigTreeNodeType::Signal;
    node->signalType = type;
    node->identifier = id.ToStdString();
    return node;
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




    wxDialog dlg(this, wxID_ANY, "Create");



    wxTextCtrl* idCtrl = nullptr;
    wxTextCtrl* defCtrl = nullptr;
    wxChoice* gateChoice = nullptr;
    wxTextCtrl* exprCtrl = nullptr;

    auto* form = new wxFlexGridSizer(2, 6, 6);
    form->AddGrowableCol(1);

    if (type == SecondNodeType::ModuleInstance ||
        type == SecondNodeType::GateInstance) {
        form->Add(new wxStaticText(&dlg, wxID_ANY, "Identifier:"));
        idCtrl = new wxTextCtrl(&dlg, wxID_ANY);
        form->Add(idCtrl, 1, wxEXPAND);
    }

    if (type == SecondNodeType::ModuleInstance) {
        form->Add(new wxStaticText(&dlg, wxID_ANY, "Definition:"));
        defCtrl = new wxTextCtrl(&dlg, wxID_ANY);
        form->Add(defCtrl, 1, wxEXPAND);
    }

    if (type == SecondNodeType::GateInstance) {
        form->Add(new wxStaticText(&dlg, wxID_ANY, "Gate Type:"));

        wxArrayString gateTypes;
        gateTypes.Add("and");
        gateTypes.Add("nand");
        gateTypes.Add("or");
        gateTypes.Add("nor");
        gateTypes.Add("xor");
        gateTypes.Add("xnor");

        gateChoice = new wxChoice(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, gateTypes);
        gateChoice->SetSelection(0); // 默认选中 And

        form->Add(gateChoice, 1, wxEXPAND);


    }

    if (type == SecondNodeType::ContinuousAssign ||
        type == SecondNodeType::Always) {
        form->Add(new wxStaticText(&dlg, wxID_ANY, "Expression:"));
        exprCtrl = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition,
            wxSize(300, -1));
        form->Add(exprCtrl, 1, wxEXPAND);
    }

    auto* btns = dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL);

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(form, 1, wxEXPAND | wxALL, 10);
    root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    dlg.SetSizerAndFit(root);

    if (dlg.ShowModal() != wxID_OK)
        return nullptr;

    auto* node = new SecondNode();
    node->type = SigTreeNodeType::Second;
    node->secondType = type;

    if (idCtrl)
        node->identifier = idCtrl->GetValue().ToStdString();
    if (defCtrl)
        node->defIdentifier = defCtrl->GetValue().ToStdString();
    if (gateChoice)
        node->gatetype = gateChoice->GetStringSelection().ToStdString();
    if (exprCtrl)
        node->assign_expression = exprCtrl->GetValue().ToStdString();

    return node;
}

SecondNode* SigFlowTreePanel::CreateModuleInstDialog(SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Instantiate Module", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    // 1. 数据准备
    wxArrayString defNames;
    std::vector<TopNode*> defPointers;
    for (auto const& [name, defPtr] : sfTree->DefinitionTable) {
        if (defPtr->topType == TopNodeType::Module) {
            TopNode* tn = static_cast<TopNode*>(parent);
            if (name != tn->identifier) defNames.Add(name);
            defPointers.push_back(defPtr);
        }
    }

    if (defNames.IsEmpty()) {
        wxMessageBox("No Module Definitions found!", "Error", wxOK | wxICON_ERROR);
        return nullptr;
    }

    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    // --- 顶部基础信息区 (使用 FlexGrid 模拟 PropertyPanel 的两列布局) ---
    auto* formGrid = new wxFlexGridSizer(2, 8, 8);
    formGrid->AddGrowableCol(1);

    formGrid->Add(new wxStaticText(&dlg, wxID_ANY, "Identifier:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* idCtrl = new wxTextCtrl(&dlg, wxID_ANY, "u_inst_0");
    formGrid->Add(idCtrl, 1, wxEXPAND);

    formGrid->Add(new wxStaticText(&dlg, wxID_ANY, "Definition:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* defChoice = new wxChoice(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, defNames);
    defChoice->SetSelection(0);
    formGrid->Add(defChoice, 1, wxEXPAND);

    rootSizer->Add(formGrid, 0, wxEXPAND | wxALL, 10);

    // --- Ports Area (Scrolled Window) ---
    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(500, 300));
    auto* portsMainSizer = new wxBoxSizer(wxVERTICAL);

    struct PortUI { std::string id; wxTextCtrl* connCtrl; };
    std::vector<PortUI> portUIList;

    // 重构 rebuildPorts 以匹配 AddPortRowWithConn 的样式
    auto rebuildPorts = [&](int selection) {
        portsMainSizer->Clear(true);
        portUIList.clear();
        TopNode* def = defPointers[selection];

        for (const auto& p : def->ports) {
            // 1. 分割线 (匹配 AddPortRowWithConn)
            wxStaticLine* line = new wxStaticLine(scrolled, wxID_ANY);
            portsMainSizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

            // 2. 水平布局行
            wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

            // --- 方向 (只读 TextCtrl) ---
            wxTextCtrl* dirCtrl = new wxTextCtrl(scrolled, wxID_ANY, SigFlowTree::ToString(p.direction));
            dirCtrl->SetEditable(false);
            dirCtrl->SetBackgroundColour(scrolled->GetBackgroundColour()); // 与底色融为一体

            // --- 引脚名 (只读 TextCtrl) ---
            wxTextCtrl* editName = new wxTextCtrl(scrolled, wxID_ANY, p.identifier);
            editName->SetEditable(false);
            editName->SetBackgroundColour(scrolled->GetBackgroundColour());

            // --- 链接符文本 ---
            wxStaticText* colon = new wxStaticText(scrolled, wxID_ANY, "connected by");
            colon->SetForegroundColour(wxColour(120, 120, 120));

            // --- 连接的信号 (淡蓝色可编辑) ---
            wxTextCtrl* editConn = new wxTextCtrl(scrolled, wxID_ANY, "");
            editConn->SetHint("Connected Signal");
            editConn->SetBackgroundColour(wxColour(240, 248, 255)); // 统一淡蓝色

            // 3. 组合行布局 (比例分配：Dir 0, Name 1, Label 0, Conn 1)
            rowSizer->Add(dirCtrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
            rowSizer->Add(editName, 1, wxEXPAND | wxLEFT, 5);
            rowSizer->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
            rowSizer->Add(editConn, 1, wxEXPAND | wxRIGHT, 10);

            portsMainSizer->Add(rowSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);
            portUIList.push_back({ p.identifier, editConn });
        }

        scrolled->SetSizer(portsMainSizer);
        scrolled->FitInside();
        scrolled->SetScrollRate(0, 10);
        dlg.Layout();
        };

    defChoice->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { rebuildPorts(defChoice->GetSelection()); });
    rebuildPorts(0);

    rootSizer->Add(scrolled, 1, wxEXPAND | wxALL, 10);
    rootSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);

    dlg.SetSizerAndFit(rootSizer);
    dlg.SetMinSize(wxSize(550, 450));

    if (dlg.ShowModal() != wxID_OK) return nullptr;

    // --- 构造返回节点 ---
    auto* node = new SecondNode();
    node->type = SigTreeNodeType::Second;
    node->secondType = SecondNodeType::ModuleInstance;
    node->identifier = idCtrl->GetValue().ToStdString();
    node->defIdentifier = defChoice->GetStringSelection().ToStdString();
    node->Definition = defPointers[defChoice->GetSelection()];

    for (size_t i = 0; i < portUIList.size(); ++i) {
        Port p;
        p.identifier = portUIList[i].id;
        p.direction = node->Definition->ports[i].direction;
        p.conn = portUIList[i].connCtrl->GetValue().ToStdString();
        node->ports.push_back(p);
    }

    return node;
}

SecondNode* SigFlowTreePanel::CreateGateInstDialog(SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Gate Instance", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    // --- 顶部基础信息 (Identifier & Gate Type) ---
    auto* formGrid = new wxFlexGridSizer(2, 8, 8);
    formGrid->AddGrowableCol(1);

    formGrid->Add(new wxStaticText(&dlg, wxID_ANY, "Identifier:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* idCtrl = new wxTextCtrl(&dlg, wxID_ANY, "g0");
    formGrid->Add(idCtrl, 1, wxEXPAND);

    formGrid->Add(new wxStaticText(&dlg, wxID_ANY, "Gate Type:"), 0, wxALIGN_CENTER_VERTICAL);
    wxArrayString gateTypes;
    gateTypes.Add("and"); gateTypes.Add("nand"); gateTypes.Add("or");
    gateTypes.Add("nor"); gateTypes.Add("xor"); gateTypes.Add("xnor");
    gateTypes.Add("buf"); gateTypes.Add("not");
    auto* gateChoice = new wxChoice(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, gateTypes);
    gateChoice->SetSelection(0);
    formGrid->Add(gateChoice, 1, wxEXPAND);

    rootSizer->Add(formGrid, 0, wxEXPAND | wxALL, 15);

    // --- Ports Area ---
    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(500, 250));
    auto* portsMainSizer = new wxBoxSizer(wxVERTICAL);

    struct PortUI { std::string id; PortDirection dir; wxTextCtrl* connCtrl; };
    std::vector<PortUI> portUIList;

    auto rebuildGatePorts = [&]() {
        portsMainSizer->Clear(true);
        portUIList.clear();
        wxString type = gateChoice->GetStringSelection();

        struct GatePortDef { std::string id; PortDirection dir; };
        std::vector<GatePortDef> defs;

        if (type == "buf" || type == "not") {
            // 1-in -> 2-out
            defs = { {"Out1", PortDirection::Out}, {"Out2", PortDirection::Out}, {"In1", PortDirection::In} };
        }
        else {
            // 2-in -> 1-out
            defs = { {"Out1", PortDirection::Out}, {"In1", PortDirection::In}, {"In2", PortDirection::In} };
        }

        for (const auto& d : defs) {
            // 1. 分割线 (完全同步前者)
            wxStaticLine* line = new wxStaticLine(scrolled, wxID_ANY);
            portsMainSizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

            // 2. 水平布局行
            wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

            // --- 方向 (只读 TextCtrl) ---
            wxTextCtrl* dirCtrl = new wxTextCtrl(scrolled, wxID_ANY, SigFlowTree::ToString(d.dir));
            dirCtrl->SetEditable(false);
            dirCtrl->SetBackgroundColour(scrolled->GetBackgroundColour());

            // --- 端口名 (只读 TextCtrl) ---
            wxTextCtrl* editName = new wxTextCtrl(scrolled, wxID_ANY, d.id);
            editName->SetEditable(false);
            editName->SetBackgroundColour(scrolled->GetBackgroundColour());

            // --- 链接符文本 ---
            wxStaticText* colon = new wxStaticText(scrolled, wxID_ANY, "connected by");
            colon->SetForegroundColour(wxColour(120, 120, 120));

            // --- 连接的信号 (淡蓝色可编辑) ---
            wxTextCtrl* editConn = new wxTextCtrl(scrolled, wxID_ANY, "");
            editConn->SetHint("Connected Signal");
            editConn->SetBackgroundColour(wxColour(240, 248, 255)); // 统一淡蓝色

            // 3. 组合行布局 (比例分配严格一致)
            rowSizer->Add(dirCtrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
            rowSizer->Add(editName, 1, wxEXPAND | wxLEFT, 5);
            rowSizer->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
            rowSizer->Add(editConn, 1, wxEXPAND | wxRIGHT, 10);

            portsMainSizer->Add(rowSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);
            portUIList.push_back({ d.id, d.dir, editConn });
        }

        scrolled->SetSizer(portsMainSizer);
        scrolled->FitInside();
        scrolled->SetScrollRate(0, 10);
        dlg.Layout();
        };

    gateChoice->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { rebuildGatePorts(); });
    rebuildGatePorts();

    rootSizer->Add(scrolled, 1, wxEXPAND | wxALL, 10);
    rootSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);

    dlg.SetSizerAndFit(rootSizer);
    if (dlg.ShowModal() != wxID_OK) return nullptr;

    // --- 数据封装 ---
    auto* node = new SecondNode();
    node->type = SigTreeNodeType::Second;
    node->secondType = SecondNodeType::GateInstance;
    node->identifier = idCtrl->GetValue().ToStdString();
    node->gatetype = gateChoice->GetStringSelection().ToStdString();

    for (auto& ui : portUIList) {
        Port p;
        p.identifier = ui.id;
        p.direction = ui.dir;
        p.conn = ui.connCtrl->GetValue().ToStdString();
        node->ports.push_back(p);
    }
    return node;
}

SecondNode* SigFlowTreePanel::CreateContiniousAssignDialog(SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Continuous Assignment", wxDefaultPosition, wxSize(500, 600),
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    // --- 临时数据模型 ---
    std::string idVal = "assign0";
    std::string exprVal = "";
    std::vector<Port> tempPorts;
    // 默认添加一个输出端口
    tempPorts.push_back({ "Out1", PortDirection::Out, "" });

    // --- 1. 表单区域 (Identifier & Expression) ---
    auto* formPanel = new wxPanel(&dlg);
    auto* formSizer = new wxFlexGridSizer(2, 10, 10);
    formSizer->AddGrowableCol(1);

    // Expression
    formSizer->Add(new wxStaticText(formPanel, wxID_ANY, "Expression:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* exprCtrl = new wxTextCtrl(formPanel, wxID_ANY, exprVal, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    exprCtrl->SetHint("e.g. In1 & In2");
    formSizer->Add(exprCtrl, 1, wxEXPAND);

    formPanel->SetSizer(formSizer);
    rootSizer->Add(formPanel, 0, wxEXPAND | wxALL, 15);


    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(-1, 300), wxVSCROLL);
    auto* portsMainSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsMainSizer);
    scrolled->SetScrollRate(0, 20);
    rootSizer->Add(scrolled, 1, wxEXPAND | wxALL, 10);

    // --- 3. 动态刷新 Ports 的 Lambda (参考 AddPortContinuousAssign 逻辑) ---
    auto rebuildPortsUI = [&]() {
        portsMainSizer->Clear(true);
        for (auto& p : tempPorts) {
            auto* itemWrapper = new wxBoxSizer(wxVERTICAL);
            auto* contentRow = new wxBoxSizer(wxHORIZONTAL);

            auto* dirC = new wxTextCtrl(scrolled, wxID_ANY, SigFlowTree::ToString(p.direction));
            dirC->SetEditable(false);
            dirC->SetBackgroundColour(scrolled->GetBackgroundColour());

            auto* editName = new wxTextCtrl(scrolled, wxID_ANY, p.identifier);
            editName->SetEditable(false);
            editName->SetBackgroundColour(scrolled->GetBackgroundColour());

            auto* colon = new wxStaticText(scrolled, wxID_ANY, "connected by");
            colon->SetForegroundColour(wxColour(120, 120, 120));

            // 注意：对话框内实时更新 tempPorts 数据
            auto* editConn = new wxTextCtrl(scrolled, wxID_ANY, p.conn);
            editConn->SetHint("Connected Signal");
            editConn->SetBackgroundColour(wxColour(240, 248, 255));

            // 绑定实时更新到 tempPorts
            editConn->Bind(wxEVT_TEXT, [&](wxCommandEvent& e) {
                // 通过指针查找对应的 p 可能不安全（vector 扩容），这里用 item index 绑定更稳
                // 但简便起见，对话框内可以用这种方式更新
                p.conn = e.GetString().ToStdString();
                });

            contentRow->Add(dirC, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
            contentRow->Add(editName, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
            contentRow->Add(colon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
            contentRow->Add(editConn, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

            itemWrapper->Add(contentRow, 0, wxEXPAND | wxTOP | wxBOTTOM, 5);
            itemWrapper->Add(new wxStaticLine(scrolled, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
            portsMainSizer->Add(itemWrapper, 0, wxEXPAND);
        }
        scrolled->Layout();
        scrolled->FitInside();
        };

    rebuildPortsUI();

    // --- 4. 增删按钮组 ---
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* addBtn = new wxButton(&dlg, wxID_ANY, "+ Add Port");
    auto* delBtn = new wxButton(&dlg, wxID_ANY, "- Delete Port");
    delBtn->SetForegroundColour(wxColour(200, 0, 0));

    addBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        Port p;
        p.direction = PortDirection::In;
        p.identifier = "In" + std::to_string(tempPorts.size()); // 简易编号
        tempPorts.push_back(p);
        rebuildPortsUI();
        });

    delBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        if (tempPorts.size() > 1) {
            tempPorts.pop_back();
            rebuildPortsUI();
        }
        });

    btnSizer->Add(addBtn, 0, wxRIGHT, 10);
    btnSizer->Add(delBtn, 0);
    rootSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 10);

    // --- 5. 标准对话框按钮 ---
    rootSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);

    dlg.SetSizer(rootSizer);
    if (dlg.ShowModal() != wxID_OK) return nullptr;

    // --- 构造最终节点 ---
    auto* node = new SecondNode();
    node->type = SigTreeNodeType::Second;
    node->secondType = SecondNodeType::ContinuousAssign;
    node->assign_expression = exprCtrl->GetValue().ToStdString();
    node->ports = tempPorts;

    return node;
}
