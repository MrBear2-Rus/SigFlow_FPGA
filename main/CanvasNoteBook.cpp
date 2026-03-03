#include "CanvasNoteBook.h"
#include "MainFrame.h"

#include <wx/aui/auibar.h>
//新增：绑定事件表
wxBEGIN_EVENT_TABLE(CanvasNoteBook, wxAuiNotebook)
EVT_COMMAND(wxID_ANY, wxEVT_CANVAS_MODIFIED, CanvasNoteBook::OnCanvasModified)
wxEND_EVENT_TABLE()

CanvasNoteBook::CanvasNoteBook(MainFrame* pa, SigFlowTree* sftree, wxWindowID id, size_t size_x, size_t size_y)
    : wxAuiNotebook(pa, id, wxDefaultPosition, wxDefaultSize, wxAUI_NB_DEFAULT_STYLE),
    mf(pa), sftree(sftree), size_x(size_x), size_y(size_y)
{
    //AddCanvasPage(new CanvasPanel(this, sftree, size_x, size_y), "untitled", true);
    //AddCustomButton();
    Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &CanvasNoteBook::OnPageClose, this);
}

bool CanvasNoteBook::AddCanvasPage(CanvasPanel* panel, const wxString& caption, bool select) {
    // 1. 同步数据容器
    cvses.push_back(panel);

    // 2. 调用基类实际执行 UI 操作
    return AddPage(panel, caption, select);
}

//新增： 实现 OnCanvasModified：接收子面板修改事件
void CanvasNoteBook::OnCanvasModified(wxCommandEvent& evt) {
    // 标记 NoteBook 为待保存
    m_isModified = true;

    // 可选：打印日志，确认收到哪个画布的修改事件
    wxString canvasId = evt.GetString();
    SetStatusText(wxString::Format("画布 %s 已修改", canvasId), 0);

    evt.Skip();
}
bool CanvasNoteBook::RemoveCanvasPage(size_t page_idx) {
    if (page_idx >= cvses.size()) return false;

    // 1. 删除对应的 CanvasPanel 指针
    // 注意：DeletePage 会自动 delete 掉 Panel 对象，所以这里先移除指针
    cvses.erase(cvses.begin() + page_idx);

    // 2. 调用基类执行 UI 操作
    return DeletePage(page_idx);
}

void CanvasNoteBook::DeleteAll() {
    for (int i = 0; i < cvses.size(); i++) {
        RemoveCanvasPage(i);
    }
}


void CanvasNoteBook::UpdateNoteBook() {
    DeleteAll();
    bool sel = true;
    for (auto* cld : fn->GetChildren()) {
        TopNode* tn = static_cast<TopNode*>(cld);
        CanvasPanel* ca = new CanvasPanel(this, sftree, this->size_x, this->size_y);
        ca->SetTopNode(tn);
        AddCanvasPage(ca, ca->GetNote(), sel);
        sel = false;
    }
    AdjustScaleToFit();
    //DeleteAddButton();
    AddCustomButton();
}

void CanvasNoteBook::SetStatusText(const wxString& text, int number) {
    if(mf) mf->SetStatusText(text, number);
}

void CanvasNoteBook::AdjustScaleToFit() {
    for (auto* c : cvses) {
        c->SetScale(1.0);
        c->SetoffSet(wxPoint(0, 0));
    }
}

//修改：保存后重置isModified
void CanvasNoteBook::SaveOrNotWindow() {
    // 2. 弹出提示框询问用户
    if (!fn) return;
    wxMessageDialog dial(this,
        wxString::Format("save %s?", fn->GetName()),
        "save",
        wxYES_NO | wxCANCEL | wxICON_QUESTION);

    int result = dial.ShowModal();

    if (result == wxID_YES) {
        
        for (auto* c : cvses) {
            c->Save(); // CanvasPanel::Save() 会重置自身 isModified
        }
        // 新增：重置 NoteBook 的修改标记
        m_isModified = false;
        SetStatusText("已保存所有画布", 0);
    }
    else if (result == wxID_NO) {
        // 不保存，直接重置标记（可选，根据你的需求）
        m_isModified = false;
    }
    else if (result == wxID_CANCEL) {
        // 取消操作，不修改标记
        return;
    }

}

void CanvasNoteBook::SetCurrentComponent(wxString type) {
    CanvasPanel* sel = GetSelectedPage();
    if (sel) sel->SetCurrentComponent(type);
}

CanvasPanel* CanvasNoteBook::GetSelectedPage() {
    int sel = GetSelection();
    if (sel != wxNOT_FOUND) {
        wxWindow* page = GetPage(sel);
        // 如果你想直接拿到你的 CanvasPanel 指针：
        CanvasPanel* currentCanvas = wxDynamicCast(page, CanvasPanel);
        return currentCanvas;
    }
    else return nullptr;
}


void CanvasNoteBook::SigFlowNodeAdded(SigTreeNode* n) {
    switch (n->type) {
    case SigTreeNodeType::Top: {
        TopNode* tn = static_cast<TopNode*>(n);
        if (tn->GetParent() == fn) {
            CanvasPanel* c = new CanvasPanel(this, sftree, size_x, size_y);
            c->SetTopNode(tn);
            AddCanvasPage(c, tn->identifier, true);
            AdjustScaleToFit();
        }
        break;
        }
    case SigTreeNodeType::Second: {
        SecondNode* sn = static_cast<SecondNode*>(n);
        for (int i = 0; i < cvses.size(); i++) {
            auto* page = cvses[i];
            if (page->tn == sn->GetParent()) {

                page->AddSecondElement(sn);
            }
        }
    }
    }
}

void CanvasNoteBook::SigFlowNodeDeleted(SigTreeNode* n) {
    switch (n->type) {
    case SigTreeNodeType::Top: {
        for (int i = 0; i < cvses.size(); i++) {
            auto* page = cvses[i];
            if (page->tn == n) RemoveCanvasPage(i);
        }
        break;
    }
    case SigTreeNodeType::Second: {
        SecondNode* sn = static_cast<SecondNode*>(n);
        for (int i = 0; i < cvses.size(); i++) {
            auto* page = cvses[i];
            if (page->tn == sn->GetParent()) {
                page->DelSecondElement(sn);
            }
        }
        break;
    }
    }
}

void CanvasNoteBook::SigFlowNodeChanged(SigTreeNode* n) {
    switch (n->type) {
    case SigTreeNodeType::Top: {
        for (int i = 0; i < cvses.size(); i++) {
            auto* page = cvses[i];
            if (page->tn == n) {
                page->LoadLayout();
                this->SetPageText(i, page->GetNote());
            }
        }
        break;
    }
    case SigTreeNodeType::Second: {
        SecondNode* sn = static_cast<SecondNode*>(n);
        for (int i = 0; i < cvses.size(); i++) {
            auto* page = cvses[i];
            if (page->tn == sn->GetParent()) {
                page->RefreshElem(sn);
            }
        }
        break;
    }
    }
}


void CanvasNoteBook::AddCustomButton() {
    // 1. 创建按钮，父窗口设为 CanvasNoteBook 自身 (this)
    wxWindowList& children = GetChildren();
    wxWindow* tabCtrl = nullptr;
    for (wxWindow* child : children) {
        if (child->GetClassInfo()->GetClassName() == wxString("wxAuiTabCtrl")) {
            tabCtrl = child;
            break;
        }
    }

    if (!tabCtrl) return;

    wxButton* m_addBtn = new wxButton(tabCtrl, wxID_ANY, "+",
        wxDefaultPosition, wxSize(24, 24), // 略微放大
        wxBORDER_SIMPLE); // 使用简单的边框

    m_addBtn->SetFont(wxFont(12, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
    m_addBtn->SetForegroundColour(*wxBLACK);

    tabCtrl->Bind(wxEVT_PAINT, [m_addBtn](wxPaintEvent& evt) {
        evt.Skip(); // 让系统先画标签栏
        m_addBtn->Refresh(); // 强迫按钮在标签栏画完后跟着重画，防止被覆盖
        });

    // 2. 绑定点击
    m_addBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        this->OnAddNewPageRequested();
        });

    // 3. 稳健定位：将按钮提升到顶部 (Z-order)
    m_addBtn->Raise();
    wxSize sz = this->GetClientSize();
    m_addBtn->SetPosition(wxPoint(sz.x-30, 3));
    // 4. 监听 Notebook 的大小变化，而不是 TabCtrl
    this->Bind(wxEVT_SIZE, [this, m_addBtn](wxSizeEvent& evt) {
        wxSize sz = this->GetClientSize();

        // --- 核心定位逻辑 ---
        // 避开 Scroll 按钮和关闭按钮，将其放在标签栏的右上角
        // 宽度 30px 通常足以避开左侧的 Tab，在最右侧的按钮左边
        int xPos = sz.x - 30;
        int yPos = 3; // 标签栏通常高度较小，放在顶部下方即可

        if (m_addBtn)  m_addBtn->SetPosition(wxPoint(xPos, yPos));

        evt.Skip();
        });
}

void CanvasNoteBook::DeleteAddButton() {
    if (m_addBtn) {
        // wxWidgets 规定：销毁窗口必须用 Destroy() 以确保事件队列安全
        m_addBtn->Destroy();
        m_addBtn = nullptr; // 避免野指针
    }
}

void CanvasNoteBook::OnAddNewPageRequested() {
    // 后台逻辑：具体添加什么由后台决定
    TopNode* tn = new TopNode("new_module", TopNodeType::Module);
    tn = static_cast<TopNode*>(sftree->AddChild(fn, tn));
}

void CanvasNoteBook::OnPageClose(wxAuiNotebookEvent& evt) {
    // 1. 获取被关闭页面的索引
    int sel = evt.GetSelection();

    // 2. 根据索引找到对应的 CanvasPanel 实例
    CanvasPanel* panel = (CanvasPanel*)GetPage(sel);
    TopNode* tn = panel->tn;

    wxMessageDialog dlg(this, wxString::Format("Are you sure you want to delete Module %s?", tn->identifier),
        "Confirm Deletion", wxYES_NO | wxICON_QUESTION);

    if (dlg.ShowModal() == wxID_NO) {
        // Veto the event to prevent the page from being closed
        evt.Veto();
        return;
    }

    sftree->RemoveChild(fn,tn);


    /*
    // 3. 自定义逻辑：检查是否有未保存的更改
    if (panel && panel->IsModified()) {
        wxMessageDialog dlg(this, "画布有未保存的修改，确定要关闭吗？",
            "确认关闭", wxYES_NO | wxICON_QUESTION);

        if (dlg.ShowModal() == wxID_NO) {
            // --- 核心点：如果不满足自定义条件，Veto() 事件 ---
            // 这样，内置的关闭叉叉就不会删除该页面
            evt.Veto();
            return;
        }
    }*/

    // 4. 如果用户选择确定，或者没有修改，可以手动执行清理逻辑
    // 如：移除 cvses vector 中的记录，通知逻辑层删除节点等
    //this->RemoveCanvasFromTracking(panel);

    // evt.Skip() 会允许默认的删除行为执行
    evt.Skip();
}
