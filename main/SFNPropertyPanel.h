#pragma once
#include <wx/wx.h>
#include "SigTree.h"
#include <wx/statline.h>

wxDECLARE_EVENT(EVT_SFTREE_CHANGED, wxCommandEvent);


class SFNPropertyPanel :public wxScrolledWindow {
public:
    SigTreeNode* m_node;
    SigFlowTree* m_tree;

    wxBoxSizer* m_mainSizer; // 主垂直布局 
    wxFlexGridSizer* m_formSizer; // 二列属性布局

    bool m_reloading;
    bool m_reloadRequested;
    // 内部布置表单的辅助接口

    void AddSectionTitle(const wxString& title);
    void AddTextRow(const wxString& label, const wxString& value);
    void AddChangeTextRow(const wxString& label, std::string& value);
    //void AddComboRow(const wxString& label, const wxArrayString& choices, int selection);
    void AddPortRow(std::vector<Port>& ps, PortDirection pd);
    void AddPortContinuousAssign(std::vector<Port>& in_ps, std::vector<Port>& out_ps);
    void AddPortRowWithConn(const wxString& name, PortDirection dir, std::string& conn);
    void AddChoicesRow(const wxString& label,
        std::string& boundValue,
        const wxArrayString& choices);

    void Add_BN_OR_B_Expressions(AlwaysNode* an);
    void Add_BN_OR_B_Expression(wxSizer* groupSizer, std::vector<Port>& ports, NB_OR_B_Expression& exp);
    void Add_BN_OR_B_Ports(wxSizer* groupSizer, AlwaysNode* an, std::vector<Port>& in_ports, std::vector<Port>& out_ports, int exp_id);
    void Add_BN_OR_B_Port(wxSizer* groupSizer, Port& p);

    void ShowChangePortsList(std::vector<Port>& ports);
    void ClearForm(); // 清空旧控件


    SFNPropertyPanel(wxWindow* parent, SigFlowTree* tree);
    void LoadNode(SigTreeNode* node);
    void Fresh();
    void Fresh_Self();
};
