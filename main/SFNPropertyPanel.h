#pragma once
#include <wx/wx.h>
#include "SigTree.h"
#include <wx/statline.h>

wxDECLARE_EVENT(EVT_SFTREE_CHANGED, wxCommandEvent);

class SFNPropertyPanel : public wxScrolledWindow {
public:
    SigTreeNode* m_node;
    SigFlowTree* m_tree;
    wxBoxSizer* m_mainSizer;
    bool m_reloading;
    bool m_reloadRequested;

    SFNPropertyPanel(wxWindow* parent, SigFlowTree* tree);
    void LoadNode(SigTreeNode* node);
    void Fresh();
    void Fresh_Self();

private:
    void ClearForm();

    wxSizer* CreateTopPortsSizer(TopNode* tn, PortDirection dir);
    wxSizer* CreateSecondPortRowSizer(SecondNode* sn, const wxString& name, PortDirection dir, std::string& conn);
    wxSizer* CreateContinuousAssignPortsSizer(ContinuousAssignNode* cn);

    wxSizer* Add_BN_OR_B_Expressions(AlwaysNode* an);
    void Add_BN_OR_B_Expression(wxSizer* groupSizer, AlwaysNode* an, AlwaysStatement* stmt, size_t expIndex);
    void Add_BN_OR_B_Ports(wxSizer* groupSizer, AlwaysNode* an, AlwaysStatement* stmt, int exp_id);
    void Add_BN_OR_B_Port(wxSizer* groupSizer, AlwaysNode* an, Port& p);
};
