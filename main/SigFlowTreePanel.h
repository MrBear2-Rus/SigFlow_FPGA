#pragma once
#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/event.h>

#include "SigTree.h"
#include "ui/UiTheme.h"   // T-140：主题订阅句柄（析构自动解绑）

wxDECLARE_EVENT(EVT_SFTREE_NODE_ACTIVATED, wxCommandEvent);

class SigTreeItemData : public wxTreeItemData {
public:
    SigTreeNode* node;
    SigTreeItemData(SigTreeNode* n) : node(n) {}
};




class SigFlowTreePanel : public wxPanel {
public:
    SigFlowTree* sfTree;
    FileNode* fn = nullptr;
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

    void InitTreeIcons();

    TopNode* ShowCreateTopDialog(TopNodeType type, SigTreeNode* parent);
    SignalNode* ShowCreateSignalDialog(SignalType type, SigTreeNode* parent);
    SecondNode* ShowCreateSecondDialog(SecondNodeType type, SigTreeNode* parent);
    SecondNode* CreateModuleInstDialog(SigTreeNode* parent);
    SecondNode* CreateGateInstDialog(SigTreeNode* parent);
    SecondNode* CreateContiniousAssignDialog(SigTreeNode* parent);

    void SetFileNode(FileNode* fn) { this->fn = fn; Fresh(); };
    void OverView() { this->fn = nullptr; };
    void Fresh();

    // T-140 / THE-030：把构造期一次性 Set*Colour 抽成可重施入口。
    // 只重取 token 色 + 重写**已存在**的树项色，不重建树、不改数据模型/事件。
    void ReapplyTheme();

private:
    UiTheme::Subscription m_themeSubscription;
};

