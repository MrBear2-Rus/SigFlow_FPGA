#pragma once
#include <wx/wx.h>
#include <wx/treectrl.h>
#include <map>
#include <vector>

// T-140：主题订阅句柄是成员（析构自动解绑，THE-028）。
#include "ui/UiTheme.h"

class ToolboxPanel : public wxPanel
{
public:
    explicit ToolboxPanel(wxWindow* parent);
    void Rebuild();                 // 外部调用，重建工具树

    // 添加公共访问方法
    wxTreeCtrl* GetTree() const { return m_tree; }

    void AddDefinition(wxString defId);
    void DelDefinition(wxString defId);

private:
    wxTreeCtrl* m_tree;
    wxImageList* m_imgList;

    // T-140 / THE-030：把构造期一次性 Set*Colour 抽成可重施入口；
    // 回调只做取色 + Refresh（不重布局、不改数据模型）。定义在 .cpp 文件尾部。
    void ReapplyTheme();

    // 原有函数声明
    void LoadToolIcon(const wxString& toolName, const wxString& svgFileName);
    int GetToolIconIndex(const wxString& toolName);
    void OnItemActivated(wxTreeEvent& evt);
    void OnBeginDrag(wxTreeEvent& evt);
    void OnToolSelected(wxTreeEvent& evt);

    std::map<wxString, wxString> m_displayToFile;  // 显示名称 -> 文件名（无扩展名）
    std::map<wxString, int> m_toolIconIndex;

    wxTreeItemId m_def;
    // T-140：5 个分类头（Input/Output、Wiring、Gates、Block、Definition）的句柄。
    // 只记录**展示用**的树项 id，不触碰树的 item data / 事件 / 数据模型。
    std::vector<wxTreeItemId> m_groupItems;

    UiTheme::Subscription m_themeSubscription;

    wxDECLARE_EVENT_TABLE();
};
