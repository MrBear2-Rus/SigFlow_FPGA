#pragma once
#include <wx/aui/auibook.h>
#include <wx/wx.h>
#include "CanvasPanel.h"
#include "SigTree.h"
class MainFrame;

class CanvasNoteBook : public wxAuiNotebook {
public:
    MainFrame* mf;
    SigFlowTree* sftree;
    FileNode* fn;
    std::vector<CanvasPanel*> cvses;
    size_t size_x, size_y;

    wxButton* m_addBtn = nullptr;

    CanvasNoteBook(MainFrame* pa, SigFlowTree* sftree, wxWindowID id, size_t size_x, size_t size_y);
    void SetFileNode(FileNode* node) {
        fn = node; UpdateNoteBook
        ();
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
    void SigFlowNodeChanged(SigTreeNode* n);
};
