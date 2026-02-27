#pragma once
#include <wx/aui/auibook.h>
#include <wx/wx.h>
#include "CanvasPanel.h"
#include "SigTree.h"
class MainFrame;

class CanvasNoteBook : public wxAuiNotebook {
public:
    MainFrame* mf;
    FileNode* fn;
    std::vector<CanvasPanel*> cvses;
    size_t size_x, size_y;

    CanvasNoteBook(MainFrame* pa, wxWindowID id, size_t size_x, size_t size_y);
    void SetFileNode(FileNode* node) {
        fn = node; UpdateNoteBook
        ();
    };

    bool AddCanvasPage(CanvasPanel* panel, const wxString& caption, bool select = false);
    bool RemoveCanvasPage(size_t page_idx);

    void UpdateNoteBook();
    void DeleteAll();
    void SetStatusText(const wxString& text, int number = 0);

    void AdjustScaleToFit();
    void SaveOrNotWindow();
};
