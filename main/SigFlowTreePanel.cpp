#include "SigFlowTreePanel.h"
#include "platform/PlatformPaths.h"
#include "PropertyPanelBuilder.h"
#include <wx/statline.h>
#include <wx/artprov.h>

// T-310：逻辑树字号/配色一律经唯一 Token 层取用（红线 SAFE-012 / THE-001）。
#include "ui/UiTheme.h"

namespace {

// T-310 / CMP-035：树项 12 DIP；空状态说明 12 DIP（DES-015 一句说明文案）。
wxFont SigTreeItemFont(const wxWindow* window)
{
    return UiTheme::UiFont(window, UiTheme::TextSize::P12, UiTheme::Weight::Normal);
}

// 应用整棵树的 chrome 配色（本面板树节点改造前取色 = 0，全部走系统默认）。
// 说明：wxTreeCtrl 是原生控件，没有 hover 态入口，选中底也不能指定为 `sel`/`hover`
// token（MSW 在 CDIS_SELECTED 时以系统高亮覆盖逐项底色），详见证据文件「未实现」清单。
void ApplySigTreeLook(wxTreeCtrl* tree)
{
    if (!tree) {
        return;
    }

    const UiTheme::Colours& colours = UiTheme::Palette();
    tree->SetBackgroundColour(wxColour(255, 255, 255));
    tree->SetForegroundColour(colours.text1);
    tree->SetFont(SigTreeItemFont(tree));
    tree->SetIndent(tree->FromDIP(UiTheme::Spacing::S2));  // CMP-035 缩进：8 DIP
}

// CMP-040 / DES-015 / INT-016：无工程（或逻辑树尚未建立根）时不留纯空白，
// 用一行 text2 / 12 DIP 的说明文案占位。占位项不带 SigTreeItemData，
// 因此不会进入节点激活/新增/删除等业务分支。
void ShowSigTreeEmptyState(wxTreeCtrl* tree, const wxString& message)
{
    if (!tree) {
        return;
    }

    const wxTreeItemId emptyItem = tree->AddRoot(message, -1, -1);
    tree->SetItemTextColour(emptyItem, UiTheme::Palette().text2);
    tree->SetItemFont(emptyItem, SigTreeItemFont(tree));
}

// T-310 / SAFE-012：对话框按钮取色改由 token 提供（改前为字面量蓝 / 字面量红，
// 见证据 `docs/GUI-CHUGAO/_evidence/T310-sidebar-token-map.md` §2 #6-#9）。
// 主辑：动作蓝原值为饱和蓝 rgb(0,120,215) → 对应 token 是 **accent 本身**；
// 不要用焦点环合成色（= accent 45% 合成于 bgPanel 的浅蓝 #87baee）——它作为按钮文字
// 会发白、对比度不足（THE-023）。删除语义用 danger。
wxColour DialogActionColour()
{
    return UiTheme::Palette().accent;
}

wxColour DialogDangerColour()
{
    return UiTheme::Palette().danger;
}

// T-310 / CMP-035：树面板上的 Add/Delete 按钮统一中性底 + 语义前景
//（与 T-330 `ApplyPropertyPanelLook` 同一规格，不改动按钮文案与事件绑定）。
void ApplySigPanelButtonLook(wxButton* button, const wxColour& foreground)
{
    if (!button) {
        return;
    }

    button->SetFont(UiTheme::UiFont(button, UiTheme::TextSize::P12, UiTheme::Weight::Normal));
    button->SetBackgroundColour(UiTheme::Palette().strongOnPanel);
    button->SetForegroundColour(foreground);
}


// Create Module 对话框在真正创建树节点前复用的两项 Verilog 名称规则。
bool IsSimpleVerilogIdentifier(const wxString& value)
{
    if (value.IsEmpty() || !(wxIsalpha(value[0]) || value[0] == '_')) {
        return false;
    }

    for (const wxChar character : value) {
        if (!(wxIsalnum(character) || character == '_' || character == '$')) {
            return false;
        }
    }
    return true;
}

bool HasTopIdentifier(const SigTreeNode* parent, const wxString& identifier)
{
    if (!parent) {
        return false;
    }

    const std::string candidate = identifier.ToStdString();
    for (const SigTreeNode* child : parent->GetChildren()) {
        if (child && child->type == SigTreeNodeType::Top &&
            static_cast<const TopNode*>(child)->identifier == candidate) {
            return true;
        }
    }
    return false;
}

wxString MakeUniqueTopIdentifier(const SigTreeNode* parent)
{
    wxString candidate = "module_name";
    int suffix = 2;
    while (HasTopIdentifier(parent, candidate)) {
        // 保留默认名称的可读性，同时避免第二次 Add Node 产生重复模块。
        candidate = wxString::Format("module_name_%d", suffix++);
    }
    return candidate;
}

} // namespace

SigFlowTreePanel::SigFlowTreePanel(wxWindow* parent, SigFlowTree* sfTree)
    : wxPanel(parent) {
    this->sfTree = sfTree;
    tree = new wxTreeCtrl(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxTR_DEFAULT_STYLE | wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxBORDER_NONE);

    addBtn = new wxButton(this, wxID_ANY, "Add Node");
    delBtn = new wxButton(this, wxID_ANY, "Delete Node");
    // T-310：按钮中性底 + 语义前景（删除用 danger；文案与事件绑定不变）。
    ApplySigPanelButtonLook(addBtn, UiTheme::Palette().text1);
    ApplySigPanelButtonLook(delBtn, UiTheme::Palette().danger);

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

    // T-310 / CMP-035：逻辑树按 12 DIP + surfaceOnPanel 底呈现（此前为系统默认字号）。
    ApplySigTreeLook(tree);

    InitTreeIcons();
    Fresh();

    // T-140 / THE-030：构造末尾建立订阅 + 立即重施一次（首帧正确）。
    // 置于 Fresh() 之后：重施只读已有树项，不新建/删除任何节点。
    m_themeSubscription = UiTheme::Subscribe([this] {
        ReapplyTheme();
        Refresh(false);
    });
    ReapplyTheme();
}

// T-140 / THE-030：面板级主题重施入口。
// 只做取色 + 写回，不做重布局（不调 Layout / SetSize / SendSizeEvent）、不改树的数据模型。
void SigFlowTreePanel::ReapplyTheme()
{
    // 树 chrome：surfaceOnPanel 底 + text1 正文 + 字体档 + 8 DIP 缩进
    ApplySigTreeLook(tree);
    // Add/Delete 按钮：strongOnPanel 底 + 语义前景（Add = text1，Delete = danger）
    ApplySigPanelButtonLook(addBtn, UiTheme::Palette().text1);
    ApplySigPanelButtonLook(delBtn, UiTheme::Palette().danger);

    // 空状态占位项（ShowSigTreeEmptyState 只写一次文字色，需要重施）。
    // 只读已有项：不改文案、不加删节点、不触达任何业务分支。
    if (tree && tree->GetRootItem().IsOk()) {
        tree->SetItemTextColour(tree->GetRootItem(), UiTheme::Palette().text2);
    }
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
        // T-310 / CMP-040：空状态说明（DES-015），不留纯空白。
        ShowSigTreeEmptyState(tree, "No project tree loaded \xE2\x80\x94 open a project to build the tree.");
        tree->Thaw();
        return;
    }

    SigTreeNode* logicRoot = sfTree->root;
    if (!logicRoot) {
        ShowSigTreeEmptyState(tree, "No project tree loaded \xE2\x80\x94 open a project to build the tree.");
        tree->Thaw();
        return;
    }

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
    // T-310 / PLT-001：图标与行高基准由硬编码 24px 改为 24 DIP。
    const wxSize sz = tree->FromDIP(wxSize(24, 24));
    wxImageList* images = new wxImageList(sz.x, sz.y, true);

    // 添加图标（可以从艺术资源、图标文件或位图加载）
    // 这里的顺序要和上面的 enum 对应
    auto GetIcon = [&](const wxString& path) {
        wxBitmapBundle bundle = wxBitmapBundle::FromSVGFile(sigflow::platform::ResourcePath(path), sz);
        return bundle.GetBitmap(sz);
        };

    images->Add(GetIcon("res\\ui_icons\\folder.svg")); // Proj 0
    const wxBitmap folderFallback = wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, sz);
    wxBitmapBundle folderBundle = wxBitmapBundle::FromSVGFile(
        sigflow::platform::ResourcePath("res\\ui_icons\\folder.svg"), sz);
    images->Add(folderBundle.IsOk() ? folderBundle.GetBitmap(sz) : folderFallback); // File 1
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
    topSizer->Add(PropertyPanelBuilder::CreateIdentifierRow(
                      &dlg, "Identifier:", MakeUniqueTopIdentifier(parent), &idCtrl),
        0, wxEXPAND | wxALL, 10);

    auto* scrolled = new wxScrolledWindow(&dlg, wxID_ANY, wxDefaultPosition, wxSize(400, 250));
    auto* portsSizer = new wxBoxSizer(wxVERTICAL);
    scrolled->SetSizer(portsSizer);
    scrolled->SetScrollRate(0, 10);
    topSizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* addInBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add In", DialogActionColour());
    auto* addOutBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add Out", DialogActionColour());
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
                nameCtrl->Bind(wxEVT_TEXT, [&, i, nameCtrl](wxCommandEvent&) {
                    in_ports[i]->identifier = nameCtrl->GetValue().ToStdString();
                    });
            }
            if (delBtn) {
                delBtn->Bind(wxEVT_BUTTON, [&, i, delBtn](wxCommandEvent&) {
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
                nameCtrl->Bind(wxEVT_TEXT, [&, i, nameCtrl](wxCommandEvent&) {
                    out_ports[i]->identifier = nameCtrl->GetValue().ToStdString();
                    });
            }
            if (delBtn) {
                delBtn->Bind(wxEVT_BUTTON, [&, i, delBtn](wxCommandEvent&) {
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

    wxString id;
    while (true) {
        if (dlg.ShowModal() != wxID_OK) return nullptr;

        id = idCtrl->GetValue();
        id.Trim(true).Trim(false);
        if (!IsSimpleVerilogIdentifier(id)) {
            wxMessageBox("Enter a valid Verilog identifier. It must start with a letter or '_' "
                         "and contain only letters, digits, '_', or '$'.",
                         "Create Module", wxOK | wxICON_ERROR, &dlg);
            idCtrl->SetFocus();
            idCtrl->SelectAll();
            continue;
        }
        if (HasTopIdentifier(parent, id)) {
            wxMessageBox("A top-level module with this identifier already exists in the file.",
                         "Create Module", wxOK | wxICON_ERROR, &dlg);
            idCtrl->SetFocus();
            idCtrl->SelectAll();
            continue;
        }
        break;
    }

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
                connCtrl->Bind(wxEVT_TEXT, [&, i, connCtrl](wxCommandEvent& e) {
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
                connCtrl->Bind(wxEVT_TEXT, [&, i, connCtrl](wxCommandEvent& e) {
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
                connCtrl->Bind(wxEVT_TEXT, [&, i, connCtrl](wxCommandEvent& e) {
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
    auto* addBtn = PropertyPanelBuilder::CreateButton(&dlg, "+ Add Port", DialogActionColour());
    auto* delBtn = PropertyPanelBuilder::CreateButton(&dlg, "- Delete Port", DialogDangerColour());
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
                connCtrl->Bind(wxEVT_TEXT, [&, i, connCtrl](wxCommandEvent& e) {
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
                connCtrl->Bind(wxEVT_TEXT, [&, i, connCtrl](wxCommandEvent& e) {
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
