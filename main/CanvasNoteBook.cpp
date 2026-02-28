#include "CanvasNoteBook.h"
#include "MainFrame.h"

CanvasNoteBook::CanvasNoteBook(MainFrame* pa, wxWindowID id, size_t size_x, size_t size_y)
    : wxAuiNotebook(pa, id, wxDefaultPosition, wxDefaultSize, wxAUI_NB_DEFAULT_STYLE),
    mf(pa), size_x(size_x), size_y(size_y)
{
    // 1. 在堆上创建 CanvasPanel，其父窗口是 this (即 Notebook 本身)
    CanvasPanel* ca = new CanvasPanel(this, this->size_x, this->size_y);
    AddCanvasPage(ca, ca->GetNote(), true);
}

bool CanvasNoteBook::AddCanvasPage(CanvasPanel* panel, const wxString& caption, bool select) {
    // 1. 同步数据容器
    cvses.push_back(panel);

    // 2. 调用基类实际执行 UI 操作
    return AddPage(panel, caption, select);
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
        CanvasPanel* ca = new CanvasPanel(this, this->size_x, this->size_y);
        ca->SetTopNode(tn);
        AddCanvasPage(ca, ca->GetNote(), sel);
        sel = false;
    }
    AdjustScaleToFit();
}

void CanvasNoteBook::SetStatusText(const wxString& text, int number) {
    mf->SetStatusText(text, number);
}

void CanvasNoteBook::AdjustScaleToFit() {
    for (auto* c : cvses) {
        c->SetScale(1.0);
        c->SetoffSet(wxPoint(0, 0));
    }
}


void CanvasNoteBook::SaveOrNotWindow() {
    // 2. 弹出提示框询问用户
    if (!fn) return;
    wxMessageDialog dial(this,
        wxString::Format("save %s?", fn->GetName()),
        "save",
        wxYES_NO | wxCANCEL | wxICON_QUESTION);

    int result = dial.ShowModal();

    if (result == wxID_YES) {
        // 3. 用户选择保存
        for (auto* c : cvses) {
            c->Save();
        }
        
    }
    else if (result == wxID_NO) {

    }
    else if (result == wxID_CANCEL) {
        // 5. 用户取消操作，函数应该告诉外部不应关闭窗口
        // 这通常需要在这个函数返回一个 bool 值，或者在外部处理
    }

}
