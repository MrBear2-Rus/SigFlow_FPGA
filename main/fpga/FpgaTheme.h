#pragma once

#include <wx/button.h>
#include <wx/colour.h>
#include <wx/font.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

// 统一的深色主题（与 FpgaPinBindingPanel 的设计语言保持一致）。
namespace FpgaTheme {

const wxColour kBackground(30, 30, 32);        // 窗口 / 页面背景
const wxColour kHeader(22, 22, 24);            // 顶部标题栏
const wxColour kPanel(38, 38, 42);             // 卡片 / 内嵌面板
const wxColour kPanelAlt(45, 45, 50);          // 选中 / 徽标底色
const wxColour kBorder(58, 58, 64);            // 分隔线 / 边框
const wxColour kText(226, 232, 240);           // 主文字 #E2E8F0
const wxColour kTextSecondary(148, 163, 184);  // 次要文字 #94A3B8
const wxColour kTextMuted(100, 116, 139);      // 弱化文字 #64748B
const wxColour kBlue(59, 130, 246);            // 主强调
const wxColour kBlueDeep(30, 41, 59);          // 运行中行背景
const wxColour kGreen(34, 197, 94);            // 成功 / 主操作
const wxColour kAmber(234, 179, 8);            // 警告
const wxColour kRed(239, 68, 68);              // 错误
const wxColour kPurple(168, 85, 247);
const wxColour kOrange(251, 146, 60);
const wxColour kButton(51, 65, 85);            // 次级按钮底色

inline wxFont TitleFont(wxWindow* window)
{
    return wxFont(window->FromDIP(12), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                  wxFONTWEIGHT_BOLD);
}

inline wxFont SectionFont(wxWindow* window)
{
    return wxFont(window->FromDIP(9), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                  wxFONTWEIGHT_BOLD);
}

inline wxFont SmallFont(wxWindow* window)
{
    return wxFont(window->FromDIP(9), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                  wxFONTWEIGHT_NORMAL);
}

inline void StyleButton(wxButton* button, const wxColour& background,
                        const wxColour& foreground)
{
    button->SetBackgroundColour(background);
    button->SetForegroundColour(foreground);
    wxFont font = button->GetFont();
    font.MakeBold();
    button->SetFont(font);
}

inline void StyleSecondaryButton(wxButton* button)
{
    StyleButton(button, kButton, kText);
}

inline wxStaticText* MakeLabel(wxWindow* parent, const wxString& text,
                               const wxColour& colour = kText, bool bold = false)
{
    wxStaticText* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetForegroundColour(colour);
    if (bold) {
        wxFont font = label->GetFont();
        font.MakeBold();
        label->SetFont(font);
    }
    return label;
}

inline wxPanel* MakeDivider(wxWindow* parent, wxBoxSizer* layout, int margin = 0)
{
    wxPanel* divider = new wxPanel(parent);
    divider->SetBackgroundColour(kBorder);
    divider->SetMinSize(wxSize(-1, 1));
    layout->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);
    return divider;
}

} // namespace FpgaTheme
