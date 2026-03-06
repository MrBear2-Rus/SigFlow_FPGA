#pragma once
#include <wx/wx.h>
#include "SigTree.h"
#include <wx/statline.h>

wxDECLARE_EVENT(EVT_SFTREE_CHANGED, wxCommandEvent);

class SFNPropertyPanel : public wxScrolledWindow {
public:
    SigTreeNode* m_node;
    SigFlowTree* m_tree;

    wxBoxSizer* m_mainSizer; // 主垂直布局

    bool m_reloading;
    bool m_reloadRequested;

    SFNPropertyPanel(wxWindow* parent, SigFlowTree* tree);
    void LoadNode(SigTreeNode* node);
    void Fresh();
    void Fresh_Self();

private:
    void ClearForm(); // 仍保留，用于清空主 sizer

    // === 新增的私有辅助函数，用于创建复杂控件并绑定事件 ===
    wxSizer* CreateTopPortsSizer(TopNode* tn, PortDirection dir);
    wxSizer* CreateSecondPortRowSizer(SecondNode* sn, const wxString& name, PortDirection dir, std::string& conn);
    wxSizer* CreateContinuousAssignPortsSizer(ContinuousAssignNode* cn);

    // === 保留的 AlwaysNode 专用函数（内部仍使用 PropertyPanelBuilder） ===
    wxSizer* Add_BN_OR_B_Expressions(AlwaysNode* an);
    void Add_BN_OR_B_Expression(wxSizer* groupSizer, std::vector<Port>& out_ports, NB_OR_B_Expression& exp);
    void Add_BN_OR_B_Ports(wxSizer* groupSizer, AlwaysNode* an, std::vector<Port>& in_ports, std::vector<Port>& out_ports, int exp_id);
    void Add_BN_OR_B_Port(wxSizer* groupSizer, Port& p);
};
