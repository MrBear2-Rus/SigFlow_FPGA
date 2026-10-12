#pragma once
#include <wx/wx.h>
#include "SigTree.h"
#include <wx/statline.h>

// T-140：主题订阅句柄是成员（析构自动解绑，THE-028）。
#include "ui/UiTheme.h"

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

    // T-140 / THE-030：把构造期/构造路径上的一次性 Set*Colour 抽成可重施入口。
    // 只把 UiTheme token 重新写回**已存在**的控件树；不重建表单、不改业务状态、
    // 不调用 Layout()/FitInside()/SetSize()/SendSizeEvent()（避免 PERF 回归与重入）。
    void ReapplyTheme();

private:
    void ClearForm();

    // T-140：按控件类型重施一个子窗口的 token 色（递归下探，不改语义）。
    void ReapplyTheme(wxWindow* window);
    void ReapplyTheme(wxButton* button);
    void ReapplyTheme(wxTextCtrl* text);
    void ReapplyTheme(wxStaticText* label);
    void ReapplyTheme(wxStaticLine* line);
    void ReapplyTheme(wxChoice* choice);

    wxSizer* CreateTopPortsSizer(TopNode* tn, PortDirection dir);
    wxSizer* CreateSecondPortRowSizer(SecondNode* sn, const wxString& name, PortDirection dir, std::string& conn);
    wxSizer* CreateContinuousAssignPortsSizer(ContinuousAssignNode* cn);

    wxSizer* Add_BN_OR_B_Expressions(AlwaysNode* an);
    void Add_BN_OR_B_Expression(wxSizer* groupSizer, AlwaysNode* an, AlwaysStatement* stmt, size_t expIndex);
    void Add_BN_OR_B_Ports(wxSizer* groupSizer, AlwaysNode* an, AlwaysStatement* stmt, int exp_id);
    void Add_BN_OR_B_Port(wxSizer* groupSizer, AlwaysNode* an, Port& p);

    UiTheme::Subscription m_themeSubscription;
};
