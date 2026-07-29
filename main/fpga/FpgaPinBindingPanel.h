#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/listctrl.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/combobox.h>
#include <wx/checkbox.h>
#include <functional>
#include <set>
#include <vector>
#include <map>

#include "FpgaPinData.h"
#include "FpgaConstraint.h"

class SigFlowTree;
class MainFrame;

// ==================== 封装引脚可视化面板 ====================
// 自定义控件：绘制 QFN88 封装四边视图
class PackageView : public wxWindow {
public:
    PackageView(wxWindow* parent, const FpgaPinDatabase& pinDb);

    void SetSelectedPin(int pin);
    int GetSelectedPin() const { return m_selectedPin; }
    void SetHighlightedPins(const std::set<int>& pins);

    // 回调
    std::function<void(int pinNumber)> onPinClicked;

private:
    void OnPaint(wxPaintEvent& evt);
    void OnMouseDown(wxMouseEvent& evt);
    void OnMouseMove(wxMouseEvent& evt);

    const FpgaPinDatabase& m_pinDb;
    int m_selectedPin = -1;
    int m_hoveredPin = -1;
    std::set<int> m_highlightedPins;

    // 布局常量
    static constexpr int kPinSize = 14;
    static constexpr int kPinsPerSide = 22;
    static constexpr int kMargin = 40;
    static constexpr int kChipSize = 150;

    wxRect GetPinRect(int pinNumber) const;
    int HitTestPin(const wxPoint& pt) const;

    wxDECLARE_EVENT_TABLE();
};

// ==================== 端口列表 ====================
class PortListCtrl : public wxListCtrl {
public:
    PortListCtrl(wxWindow* parent);

    void PopulatePorts(const FpgaPortMap& ports,
        const ConstraintSheet& sheet);

    int GetSelectedPortBitIndex() const;

private:
    void OnItemSelected(wxListEvent& evt);
    wxDECLARE_EVENT_TABLE();
};

// ==================== 引脚绑定面板 (主面板) ====================
class FpgaPinBindingPanel : public wxScrolledWindow {
public:
    FpgaPinBindingPanel(wxWindow* parent, MainFrame* mainFrame);

    // 加载/刷新项目数据
    void LoadProject(const wxString& projectPath, const wxString& topModule);
    void Fresh();       // 刷新显示
    void ClearForm();   // 清空

private:
    MainFrame* m_mainFrame;

    // 数据
    wxString m_projectPath;
    wxString m_topModule;
    const FpgaPinDatabase& m_pinDb;
    ConstraintSheet m_sheet;
    FpgaPortMap m_ports; // name -> (dir, width)
    ConstraintValidator m_validator;

    // UI 组件
    wxStaticText* m_titleLabel;
    wxStaticText* m_statusLabel;
    PortListCtrl* m_portList;
    PackageView* m_packageView;
    wxListCtrl* m_pinTable;

    // 属性编辑器
    wxComboBox* m_ioTypeCombo;
    wxComboBox* m_driveCombo;
    wxCheckBox* m_pullUpCheck;
    wxCheckBox* m_pullDownCheck;
    wxTextCtrl* m_commentText;
    wxButton* m_bindBtn;
    wxButton* m_clearBtn;
    wxTextCtrl* m_pinNumberText;

    // 板级资源快捷按钮
    wxButton* m_ledBtn;
    wxButton* m_btnBtn;
    wxButton* m_uartBtn;
    wxButton* m_clkBtn;
    wxButton* m_spiBtn;

    // 操作按钮
    wxButton* m_generateCstBtn;
    wxButton* m_importCstBtn;
    wxButton* m_validateBtn;
    wxButton* m_saveBtn;
    wxButton* m_loadBtn;

    // 当前选中
    int m_selectedBindingIndex = -1;

    // 布局
    wxBoxSizer* m_mainSizer;
    wxBoxSizer* m_portSectionSizer;
    wxBoxSizer* m_pinTableSectionSizer;

    void BuildUI();
    void RefreshBindingsList();
    void RefreshPinTable(const wxString& filter = "");
    void RefreshStatus();
    void UpdateBindingView();

    // 事件处理
    void OnPortSelected(wxListEvent& evt);
    void OnPinTableSelected(wxListEvent& evt);
    void OnPinClicked(int pinNumber);
    void OnBindPin(wxCommandEvent& evt);
    void OnClearBinding(wxCommandEvent& evt);
    void OnPropertyChanged(wxCommandEvent& evt);
    void OnGenerateCst(wxCommandEvent& evt);
    void OnImportCst(wxCommandEvent& evt);
    void OnValidate(wxCommandEvent& evt);
    void OnSave(wxCommandEvent& evt);
    void OnBoardQuickSelect(wxCommandEvent& evt);

    // 辅助
    wxString GetConstraintsPath() const;
    wxString GetCstPath() const;
};
