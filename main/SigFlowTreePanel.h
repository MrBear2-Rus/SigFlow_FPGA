#pragma once
#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/event.h>

#include "SigTree.h"

wxDECLARE_EVENT(EVT_SFTREE_NODE_ACTIVATED, wxCommandEvent);
wxDECLARE_EVENT(EVT_SFTREE_CHANGED, wxCommandEvent);

class SigTreeItemData : public wxTreeItemData {
public:
    SigTreeNode* node;
    SigTreeItemData(SigTreeNode* n) : node(n) {}
};




class SigFlowTreePanel : public wxPanel {
public:
    SigFlowTree* sfTree;
    wxTreeCtrl* tree;

    wxButton* addBtn;
    wxButton* delBtn;

    SigFlowTreePanel(wxWindow* parent, SigFlowTree* sfTree);
    void BuildBranch(wxTreeItemId uiParent, SigTreeNode* logicParent);
    void OnAddSubNode(wxCommandEvent& event);

    void OnItemActivated(wxTreeEvent& event);
    void OnAdd(wxCommandEvent& e);
    void OnDelete(wxCommandEvent& e);
    void OnAddMenu(wxCommandEvent& e);

    TopNode* ShowCreateTopDialog(TopNodeType type, SigTreeNode* parent);
    SignalNode* ShowCreateSignalDialog(SignalType type, SigTreeNode* parent);
    SecondNode* ShowCreateSecondDialog(SecondNodeType type, SigTreeNode* parent);
    SecondNode* CreateModuleInstDialog(SigTreeNode* parent);
    SecondNode* CreateGateInstDialog(SigTreeNode* parent);
    SecondNode* CreateContiniousAssignDialog(SigTreeNode* parent);

    void Fresh();
};

