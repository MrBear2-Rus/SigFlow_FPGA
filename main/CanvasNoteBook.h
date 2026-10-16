#pragma once
#include <wx/aui/auibook.h>
#include <wx/wx.h>
#include "CanvasPanel.h"
#include "SigTree.h"
#include "ui/UiTheme.h"   // T-140：主题订阅句柄（析构自动解绑，THE-028）
class MainFrame;

class CanvasNoteBook : public wxAuiNotebook {
public:
    bool m_isModified = false; // 新增：NoteBook 级修改标记
    MainFrame* mf;
    SigFlowTree* sftree;
    FileNode* fn = nullptr;
    std::vector<CanvasPanel*> cvses;
    size_t size_x, size_y;

    

    CanvasNoteBook(MainFrame* pa, SigFlowTree* sftree, wxWindowID id, size_t size_x, size_t size_y);
    void SetFileNode(FileNode* node) {
        fn = node; UpdateNoteBook();
    };

    bool AddCanvasPage(CanvasPanel* panel, const wxString& caption, bool select = false);
    bool RemoveCanvasPage(size_t page_idx);

    void AddCustomButton();
    void DeleteAddButton();
    void OnAddNewPageRequested();
    void OnPageClose(wxAuiNotebookEvent& evt);
    void UpdateNoteBook();
    void DeleteAll();
    void SetStatusText(const wxString& text, int number = 0);

    void AdjustScaleToFit();
    /// 空画布底：未打开任何文件时也要"白底 + 网格"，不能露出笔记本的空灰底。
    void OnPaintEmpty(wxPaintEvent& event);
    bool SaveOrNotWindow();
    bool SaveModifiedCanvases();
    bool HasUnsavedChanges() const { return m_isModified; }

    void SetCurrentComponent(wxString type);
    CanvasPanel* GetSelectedPage();

    void SigFlowNodeAdded(SigTreeNode* n);
    void SigFlowNodeDeleted(SigTreeNode* n);

    // 新增：事件响应函数（接收 CanvasPanel 的修改事件）
    void OnCanvasModified(wxCommandEvent& evt);
    void SigFlowNodeChanged(SigTreeNode* n);

    wxDECLARE_EVENT_TABLE(); // 新增：事件表声明

    // T-140 / THE-030 缺口：页签栏上的 "+" 按钮颜色是**构造期一次性写入**，
    // 主题切换后不会更新（浅色档看不出，深色档会变"深壳浅按钮"）。
    // 按本仓既有模式订阅 UiTheme 并重施（只改外观，不改任何业务状态）。
    void ReapplyTheme();

private:
    bool m_btnCreated = false;
    wxButton* m_addBtn = nullptr;
    UiTheme::Subscription m_themeSubscription;
};
