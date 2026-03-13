#pragma once
#include <wx/aui/auibook.h>
#include <wx/wx.h>
#include "CanvasPanel.h"
#include "SigTree.h"
class MainFrame;

class CanvasNoteBook : public wxAuiNotebook {
public:
    bool m_isModified = false; // 新增：NoteBook 级修改标记
    MainFrame* mf;
    SigFlowTree* sftree;
    FileNode* fn;
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
    void SaveOrNotWindow();

    void SetCurrentComponent(wxString type);
    CanvasPanel* GetSelectedPage();

    void SigFlowNodeAdded(SigTreeNode* n);
    void SigFlowNodeDeleted(SigTreeNode* n);

    // 新增：事件响应函数（接收 CanvasPanel 的修改事件）
    void OnCanvasModified(wxCommandEvent& evt);
    void SigFlowNodeChanged(SigTreeNode* n);

    wxDECLARE_EVENT_TABLE(); // 新增：事件表声明

private:
    bool m_btnCreated = false;
    wxButton* m_addBtn ;
};
