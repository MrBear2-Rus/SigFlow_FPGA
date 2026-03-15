#include "SigFlowTreePanel.h"
#include "PropertyPanelBuilder.h"
#include <wx/statline.h>
#include <wx/artprov.h>

SigFlowTreePanel::SigFlowTreePanel(wxWindow* parent, SigFlowTree* sfTree)
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

    InitTreeIcons();
    Fresh();
}

void SigFlowTreePanel::BuildBranch(wxTreeItemId uiParent, SigTreeNode* logicParent) {
    for (auto* child : logicParent->GetChildren()) {
        int iconId = -1;
        switch (child->type) {
        case SigTreeNodeType::File: {
            iconId = 1;
            break;
        }
        case SigTreeNodeType::Top: {
            iconId = 2;
            break;
        }
        case SigTreeNodeType::Second: {
            iconId = 3;
            break;
        }
        case SigTreeNodeType::Signal:
            iconId = 4;
            break;
        }

        wxTreeItemId uiChild = tree->AppendItem(
            uiParent,
            child->GetName(),
            iconId, iconId,
            new SigTreeItemData(child)
        );
        
        BuildBranch(uiChild, child);
        tree->Expand(uiChild);
        /*
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
        }*/
    }
    if (logicParent->type == SigTreeNodeType::Top) {
        TopNode* tn = static_cast<TopNode*>(logicParent);
        for (auto* child : tn->signals) {
            wxTreeItemId uiChild = tree->AppendItem(
                uiParent,
                child->GetName(),
                4, 4,
                new SigTreeItemData(child)
            );
        }
    }
}

void SigFlowTreePanel::OnItemActivated(wxTreeEvent& event) {
    wxTreeItemId id = event.GetItem();
    if (!id.IsOk()) return;

    SigTreeItemData* data = static_cast<SigTreeItemData*>(tree->GetItemData(id));
    if (data && data->node) {
        wxCommandEvent evt(EVT_SFTREE_NODE_ACTIVATED, GetId());
        evt.SetClientData(static_cast<void*>(data->node));
        GetParent()->ProcessWindowEvent(evt);
    }
}

void SigFlowTreePanel::Fresh() {
    if (!tree) return;

    tree->Freeze();
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
        rootLabel = proj->GetName();
    }
    else {
        rootLabel = "Unknown Project";
    }

    wxTreeItemId uiRoot = tree->AddRoot(
        rootLabel,
        0, 0,
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

    bool isFile = parent->type == SigTreeNodeType::File;
    bool isTop = parent->type == SigTreeNodeType::Top;
    addModule->Enable(isFile);
    addWire->Enable(isTop);
    addReg->Enable(isTop);
    addInst->Enable(isTop);
    addGate->Enable(isTop);
    addAssign->Enable(isTop);
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
    case 1001:
        newNode.reset(ShowCreateTopDialog(TopNodeType::Module, parent));
        break;
    case 1002:
        newNode.reset(ShowCreateSignalDialog(SignalType::Wire, parent));
        break;
    case 1003:
        newNode.reset(ShowCreateSignalDialog(SignalType::Reg, parent));
        break;
    case 1004:
        newNode.reset(ShowCreateSecondDialog(SecondNodeType::ModuleInstance, parent));
        break;
    case 1005:
        newNode.reset(ShowCreateSecondDialog(SecondNodeType::GateInstance, parent));
        break;
    case 1006:
        newNode.reset(ShowCreateSecondDialog(SecondNodeType::ContinuousAssign, parent));
        break;
    case 1007:
        newNode.reset(ShowCreateSecondDialog(SecondNodeType::Always, parent));
        break;
    default:
        return;
    }

    if (!newNode) return;

    sfTree->AddChild(parent, newNode.get());

    Fresh();
}

void SigFlowTreePanel::InitTreeIcons() {
    wxSize sz = wxSize(24, 24);
    wxImageList* images = new wxImageList(sz.x, sz.y, true);

    // 添加图标（可以从艺术资源、图标文件或位图加载）
    // 这里的顺序要和上面的 enum 对应
    auto GetIcon = [&](const wxString& path) {
        wxBitmapBundle bundle = wxBitmapBundle::FromSVGFile(path, sz);
        return bundle.GetBitmap(sz);
        };

    images->Add(GetIcon("res\\icons\\project.svg")); // Proj 0
    images->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, wxSize(24, 24))); // File 1
    images->Add(GetIcon("res\\svg_icons\\module.svg")); // Module 2
    images->Add(GetIcon("res\\svg_icons\\secondary.svg")); // Sec 3
    images->Add(GetIcon("res\\svg\\wiring.svg")); // Sig 4
    //images->Add(GetIcon("res\\svg_icons\\register.svg")); // Reg 5
    //images->Add(GetIcon("res\\svg_icons\\register.svg")); // Logic 6

    // 将图像列表交给树控件管理
    tree->AssignImageList(images);
}

void SigFlowTreePanel::OnDelete(wxCommandEvent&) {
    wxTreeItemId sel = tree->GetSelection();
    if (!sel.IsOk()) return;

    auto* data = static_cast<SigTreeItemData*>(tree->GetItemData(sel));
    if (!data || !data->node) return;

    SigTreeNode* node = data->node;
    if (!node->GetParent()) return;

    sfTree->RemoveChild(node->GetParent(), node);

    Fresh();
}

// ==================== 对话框实现 ====================

TopNode* SigFlowTreePanel::ShowCreateTopDialog(TopNodeType type, SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Module", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    std::vector<SignalNode*> in_ports, out_ports;

    wxTextCtrl* idCtrl = nullptr;
    auto* topSizer = new wxBoxSizer(wxVERTICAL);
    topSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(&dlg, "Identifier:", "module_name", &idCtrl),
        0, wxEXPAND | wxALL, 10);

    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(400, 250));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* addInBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add In", wxColour(0, 120, 215));
    auto* addOutBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add Out", wxColour(0, 120, 215));
    btnSizer->Add(addInBtn, 0, wxRIGHT, 10);
    btnSizer->Add(addOutBtn, 0);
    topSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 10);

    topSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(topSizer);

    std::function<void()> rebuild = [&]() {
        portsSizer->Clear(true);
        for (size_t i = 0; i < in_ports.size(); ++i) {
            wxTextCtrl* nameCtrl = nullptr;
            wxButton* delBtn = nullptr;
            auto* rowWrapper = PropertyPanelBuilder::CreateTopPortRow(scrolled, "input",
                wxString::FromUTF8(in_ports[i]->identifier), &nameCtrl, &delBtn);
            if (nameCtrl) {
                nameCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent&) {
                    in_ports[i]->identifier = nameCtrl->GetValue().ToStdString();
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
        for (size_t i = 0; i < out_ports.size(); ++i) {
            wxTextCtrl* nameCtrl = nullptr;
            wxButton* delBtn = nullptr;
            auto* rowWrapper = PropertyPanelBuilder::CreateTopPortRow(scrolled, "output",
                wxString::FromUTF8(out_ports[i]->identifier), &nameCtrl, &delBtn);
            if (nameCtrl) {
                nameCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent&) {
                    out_ports[i]->identifier = nameCtrl->GetValue().ToStdString();
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

    addInBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        SignalNode* p = new SignalNode("in" + std::to_string(in_ports.size() + 1), SignalType::Wire, PortDirection::In);
        in_ports.push_back(p);
        rebuild();
        });
    addOutBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        SignalNode* p = new SignalNode("out" + std::to_string(out_ports.size() + 1), SignalType::Wire, PortDirection::Out);
        out_ports.push_back(p);
        rebuild();
        });

    if (in_ports.empty() && out_ports.empty()) {
        in_ports.push_back(new SignalNode{ "in1", SignalType::Wire, PortDirection::In });
        out_ports.push_back(new SignalNode{ "out1", SignalType::Wire, PortDirection::Out });
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
    case SecondNodeType::ModuleInstance:
        return CreateModuleInstDialog(parent);
    case SecondNodeType::GateInstance:
        return CreateGateInstDialog(parent);
    case SecondNodeType::ContinuousAssign:
        return CreateContiniousAssignDialog(parent);
    default:
        return nullptr;
    }
}

SecondNode* SigFlowTreePanel::CreateModuleInstDialog(SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Instantiate Module", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    wxArrayString defNames;
    std::vector<TopNode*> defPointers;
    for (auto const& [name, defPtr] : sfTree->DefinitionTable) {
        if (defPtr->topType == TopNodeType::Module) {
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

    std::vector<Port> in_ports, out_ports;

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

    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(500, 300));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
    topSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(topSizer);

    std::function<void(int)> rebuild = [&](int sel) {
        portsSizer->Clear(true);
        in_ports.clear();
        out_ports.clear();
        TopNode* def = defPointers[sel];

        for (const auto& p : def->GetInPorts()) {
            in_ports.push_back({ p->identifier, p->direction, "" });
        }
        for (const auto& p : def->GetOutPorts()) {
            out_ports.push_back({ p->identifier, p->direction, "" });
        }

        wxArrayString choices;
        choices.Add(""); // 允许空连接
        for (auto& sig : def->signals) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }
        for (auto& sig : def->in_ports) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }
        for (auto& sig : def->out_ports) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }

        for (size_t i = 0; i < in_ports.size(); ++i) {
            wxChoice* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(in_ports[i].direction),
                wxString::FromUTF8(in_ports[i].identifier),
                wxString::FromUTF8(in_ports[i].conn),
                choices,
                &connCtrl);
            if (connCtrl) {
                connCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent& e) {
                    in_ports[i].conn = e.GetString().ToStdString();
                    });
            }
            portsSizer->Add(wrapper, 0, wxEXPAND);
        }
        for (size_t i = 0; i < out_ports.size(); ++i) {
            wxChoice* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(out_ports[i].direction),
                wxString::FromUTF8(out_ports[i].identifier),
                wxString::FromUTF8(out_ports[i].conn),
                choices,
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
    return node;
}

SecondNode* SigFlowTreePanel::CreateGateInstDialog(SigTreeNode* parent) {
    wxDialog dlg(this, wxID_ANY, "Create Gate Instance", wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    std::vector<Port> ports;

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

    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(500, 250));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
    topSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(topSizer);

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

    std::function<void()> rebuild = [&]() {
        portsSizer->Clear(true);
        ports = getPortDefs(gateChoice->GetStringSelection());
        TopNode* def = static_cast<TopNode*>(parent);

        wxArrayString choices;
        choices.Add(""); // 允许空连接
        for (auto& sig : def->signals) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }
        for (auto& sig : def->in_ports) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }
        for (auto& sig : def->out_ports) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }

        for (size_t i = 0; i < ports.size(); ++i) {
            wxChoice* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(ports[i].direction),
                wxString::FromUTF8(ports[i].identifier),
                wxString::FromUTF8(ports[i].conn),
                choices,
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

    std::string expr;
    std::vector<Port> in_ports;
    std::vector<Port> out_ports = { {"Out1", PortDirection::Out, ""} };

    auto* topSizer = new wxBoxSizer(wxVERTICAL);

    wxTextCtrl* exprCtrl = nullptr;
    topSizer->Add(PropertyPanelBuilder::CreateEditableTextRow(&dlg, "Expression:", expr, &exprCtrl),
        0, wxEXPAND | wxALL, 10);
    if (exprCtrl) {
        exprCtrl->Bind(wxEVT_TEXT, [&](wxCommandEvent& e) { expr = e.GetString().ToStdString(); });
    }

    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(-1, 300));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* addBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add Port", wxColour(0, 120, 215));
    auto* delBtn = PropertyPanelBuilder::CreateButton(&dlg, "- Delete Port", wxColour(200, 0, 0));
    btnSizer->Add(addBtn, 0, wxRIGHT, 10);
    btnSizer->Add(delBtn, 0);
    topSizer->Add(btnSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 10);
    topSizer->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(topSizer);

    std::function<void()> rebuild = [&]() {
        portsSizer->Clear(true);
        TopNode* def = static_cast<TopNode*>(parent);

        wxArrayString choices;
        choices.Add(""); // 允许空连接
        for (auto& sig : def->signals) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }
        for (auto& sig : def->in_ports) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }
        for (auto& sig : def->out_ports) {
            choices.Add(wxString::FromUTF8(sig->identifier));
        }


        for (size_t i = 0; i < out_ports.size(); ++i) {
            wxChoice* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(out_ports[i].direction),
                wxString::FromUTF8(out_ports[i].identifier),
                wxString::FromUTF8(out_ports[i].conn),
                choices,
                &connCtrl);
            if (connCtrl) {
                connCtrl->Bind(wxEVT_TEXT, [&, i](wxCommandEvent& e) {
                    out_ports[i].conn = e.GetString().ToStdString();
                    });
            }
            portsSizer->Add(wrapper, 0, wxEXPAND);
        }
        for (size_t i = 0; i < in_ports.size(); ++i) {
            wxChoice* connCtrl = nullptr;
            auto* wrapper = PropertyPanelBuilder::CreateSecondPortRow(scrolled,
                SigFlowTree::ToString(in_ports[i].direction),
                wxString::FromUTF8(in_ports[i].identifier),
                wxString::FromUTF8(in_ports[i].conn),
                choices,
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

    rebuild();

    if (dlg.ShowModal() != wxID_OK) return nullptr;

    wxString id = out_ports[0].identifier;
    auto* node = new ContinuousAssignNode(id.ToStdString(), expr);
    node->in_ports = in_ports;
    node->out_ports = out_ports;
    return node;
}
