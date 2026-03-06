#pragma once
#include <wx/wx.h>
#include <wx/statline.h>

class PropertyPanelBuilder {
public:
    // 标题行（带分割线）
    static wxSizer* CreateSectionTitle(wxWindow* parent, const wxString& title);

    // 只读文本行（标签 + 只读文本框）
    static wxSizer* CreateTextRow(wxWindow* parent, const wxString& label, const wxString& value);

    // 可编辑标识符行（标签 + 可编辑文本框），返回文本框指针
    static wxSizer* CreateIdentifierRow(wxWindow* parent, const wxString& label, const wxString& initialValue,
        wxTextCtrl** textCtrlOut = nullptr);

    // 可编辑文本行（标签 + 可编辑文本框），返回文本框指针
    static wxSizer* CreateEditableTextRow(wxWindow* parent, const wxString& label, const wxString& initialValue,
        wxTextCtrl** textCtrlOut = nullptr);

    // 下拉选择行（标签 + wxChoice），返回 choice 指针
    static wxSizer* CreateChoiceRow(wxWindow* parent, const wxString& label, const wxArrayString& choices,
        int initialSelection, wxChoice** choiceOut = nullptr);

    // TopNode 端口行（方向只读、名称可编辑、删除按钮），返回名称文本框和删除按钮
    static wxSizer* CreateTopPortRow(wxWindow* parent, const wxString& direction, const wxString& portName,
        wxTextCtrl** nameCtrlOut = nullptr, wxButton** deleteBtnOut = nullptr);

    // SecondNode / ContinuousAssign / Always 端口行（方向只读、名称只读、连接可编辑），返回连接文本框
    static wxSizer* CreateSecondPortRow(wxWindow* parent, const wxString& direction, const wxString& portName,
        const wxString& connValue, wxTextCtrl** connCtrlOut = nullptr);

    // AlwaysNode 表达式行（delay、输出端口名只读、操作符选择、RHS 可编辑），返回各控件指针
    static wxSizer* CreateNBOrBExpressionRow(wxWindow* parent, float delay, const wxString& outPortName,
        bool isBlocking, const wxString& rhs,
        wxTextCtrl** delayCtrlOut = nullptr,
        wxChoice** opChoiceOut = nullptr,
        wxTextCtrl** rhsCtrlOut = nullptr);

    // 通用按钮创建（可指定前景色）
    static wxButton* CreateButton(wxWindow* parent, const wxString& label,
        const wxColour& foreground = wxNullColour);
};
