#pragma once
#include <wx/wx.h>
#include <wx/treectrl.h>
#include <map>
#include <vector>

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

    // 原有函数声明
    void LoadToolIcon(const wxString& toolName, const wxString& svgFileName);
    int GetToolIconIndex(const wxString& toolName);
    void OnItemActivated(wxTreeEvent& evt);
    void OnBeginDrag(wxTreeEvent& evt);
    void OnToolSelected(wxTreeEvent& evt);

    std::map<wxString, wxString> m_displayToFile;  // 显示名称 -> 文件名（无扩展名）
    std::map<wxString, int> m_toolIconIndex;

    wxTreeItemId m_def;

    wxDECLARE_EVENT_TABLE();
};
