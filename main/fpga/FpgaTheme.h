#pragma once

// ===========================================================================
// FpgaTheme — FPGA 工具窗 / 作业面板的配色**适配层**（T-140 / THE-034）
//
// 迁移前：本文件持有 16 个硬编码深色 `const wxColour`（GAP-A2「两套私有深色
// 主题」之一），与外壳渐进的 token 体系三足鼎立。
//
// 迁移后（本文件）：
//   ① **保留全部同名访问点**与语义（kBackground … kButton），调用点写法不变；
//   ② 取值改为**按需从 UiTheme 唯一 token 层解析**（SAFE-012 / THE-001），
//      即每次取用都重新读一次 `UiTheme::Palette()`，因此"主题变化后重新取色"
//      天然成立（THE-030 / DEC-11 恢复条件 ②）；
//   ③ 确实无对应 token 的 **EDA 领域语义色**（Bank 1 分色、选中高亮环）保留
//      字面量，但按 SAFE-012 的例外条款**只允许出现在本文件**，其它文件
//      一律经同名访问点取用；
//   ④ 访问点由 `const` 变量改为 inline 函数，与本文件既有的
//      TitleFont / SectionFont / SmallFont 同形（无需改动任何调用点语法）。
//
// 语义不变：FPGA 工具窗仍是"作业列表 + 状态语义色"的设计语言，语义色
// （成功/警告/错误/强调/运行中）全部保留；只有**底色 / 文字 / 描边**改由
// 生效档的 token 决定（浅色档 → SPEC §3.3 浅色 token；DEC-11 放开深色档后
// → 深色 token）。
//
// 未迁移范围（逐项）见 docs/GUI-CHUGAO/_evidence/T140-theme-adapter-map.md。
// ===========================================================================

#include <wx/button.h>
#include <wx/colour.h>
#include <wx/font.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <functional>
#include <vector>

#include "ui/UiTheme.h"

namespace FpgaTheme {

// ---------------------------------------------------------------------------
// 适配层入口
// ---------------------------------------------------------------------------

/// T-140 适配层入口（THE-034）：FPGA 工作区当前生效的 token 集。
///
/// 唯一取色来源，故任何"重新取色"只需再次调用本函数。
/// 若产品日后裁决"FPGA 工作区永远保持深色"，只需把 `UiTheme::Palette()`
/// 换成 `UiTheme::TerminalPalette()`（§3.3 固定深色档），其余代码零改动。
inline const UiTheme::Colours& AdaptFromUiTheme()
{
    return UiTheme::Palette();
}

// ---------------------------------------------------------------------------
// 底色 / 描边 / 文字（有对应 token —— 全部接到 UiTheme）
// ---------------------------------------------------------------------------

/// 窗口 / 页面背景（迁移前 (30,30,32)）→ `bgWindow`：窗口本体底。
inline wxColour kBackground() { return AdaptFromUiTheme().bgWindow; }

/// 顶部标题栏（迁移前 (22,22,24)）→ `surfaceOnWindow`：外壳顶栏 / 工具条同族，
/// 浅色档 #F7F7F9 比窗口底 #FBFBFD 略沉、深色档 #17181B 比 #0A0B0E 略浮，
/// 两档都能与窗口底区分（旧表的"header 与 background 可辨"关系保留）。
inline wxColour kHeader() { return AdaptFromUiTheme().surfaceOnWindow; }

/// 卡片 / 内嵌面板（迁移前 (38,38,42)）→ `bgPanel`：面板 / 卡片底。
inline wxColour kPanel() { return AdaptFromUiTheme().bgPanel; }

/// 内嵌输入 / 徽标 / 次级底（迁移前 (45,45,50)）→ `sunkenOnPanel`：
/// 消费点均为"下沉"面（ReadOnly 文本预览、位流路径框、过滤器下拉、徽标），
/// 两档 token 都比 `kPanel` 更沉，符合下沉语义。
inline wxColour kPanelAlt() { return AdaptFromUiTheme().sunkenOnPanel; }

/// 分隔线 / 边框（迁移前 (58,58,64)）→ `strokeOnPanel`。
inline wxColour kBorder() { return AdaptFromUiTheme().strokeOnPanel; }

/// 主文字（迁移前 #E2E8F0）→ `text1`。
inline wxColour kText() { return AdaptFromUiTheme().text1; }

/// 次要文字（迁移前 #94A3B8）→ `text2`。
inline wxColour kTextSecondary() { return AdaptFromUiTheme().text2; }

/// 弱化文字（迁移前 #64748B）→ `text3`。
inline wxColour kTextMuted() { return AdaptFromUiTheme().text3; }

/// 次级按钮实底（迁移前 (51,65,85)）→ `strongOnPanel`：
/// 与 `kPanel` 可区分的"更实"表面，浅色 #FEFEFE / 深色 #24252A，配 `kText` 前景。
inline wxColour kButton() { return AdaptFromUiTheme().strongOnPanel; }

// ---------------------------------------------------------------------------
// 状态 / 动作语义色（有对应 token —— 保留语义，取值接 token 语义族）
// ---------------------------------------------------------------------------

/// 主强调 / 进行中动作（迁移前 (59,130,246)）→ `accent`（行动蓝）。
inline wxColour kBlue() { return AdaptFromUiTheme().accent; }

/// 运行中行背景（迁移前 (30,41,59)）→ `selOnPanel`：选中 / 运行中实色底。
inline wxColour kBlueDeep() { return AdaptFromUiTheme().selOnPanel; }

/// 成功 / 主操作（迁移前 (34,197,94)）→ `brand`。
inline wxColour kGreen() { return AdaptFromUiTheme().brand; }

/// 警告（迁移前 (234,179,8)）→ `warn`。
inline wxColour kAmber() { return AdaptFromUiTheme().warn; }

/// 错误（迁移前 (239,68,68)）→ `danger`。
inline wxColour kRed() { return AdaptFromUiTheme().danger; }

/// 强调底（brand / accent / warn / danger）上的**前景文字**（THE-023 可读性）。
///
/// 迁移前调用点写死 `*wxBLACK` / `*wxWHITE`，无法随档位翻转：token 语义色在
/// 浅色档是深色（如 brand #1F8A4C），在深色档是亮色（#3ED37C）。取 `canvas`
/// 恰好是强调色的对比侧（浅色档 #FFFFFF / 深色档 #141519），且有对应 token，
/// 不是新增字面量。
inline wxColour kOnAccent() { return AdaptFromUiTheme().canvas; }

// ---------------------------------------------------------------------------
// 保留语义色（**无对应 token**，按 THE-034 / 用户裁决集中在本文件）
// ---------------------------------------------------------------------------

/// Bank 1 分色（迁移前 (168,85,247)）。
///
/// 理由：§3.3 基础族与 DEC-02 扩展族都没有"EDA Bank 分色"这一类 token；
/// 它是与 Bank 0(`kBlue`) / Bank 2(`kAmber`) 成组的**引脚分区语义色**，
/// 换成任何既有 token 都会与相邻 Bank 撞色。故保留字面量，并按 SAFE-012
/// 例外条款**只允许出现在本文件**（消费点 `FpgaPinBindingPanel`）。
inline wxColour kPurple() { return wxColour(168, 85, 247); }

/// 选中 / 悬停高亮环（迁移前 (251,146,60)）。
///
/// 理由：语义是"当前选中指示"（引脚环、包视图选中框），与 `warn`（告警琥珀）
/// 语义不同且必须可同时出现（例如选中的引脚恰好处于告警状态），故不并入 warn；
/// 同样集中在本文件。
inline wxColour kOrange() { return wxColour(251, 146, 60); }

// ---------------------------------------------------------------------------
// 主题刷新（THE-030 / DEC-11 恢复条件 ②）
//
// 窗口构造时把每个"重新取色"动作登记到自己的 RepaintActions 表；UiTheme
// 变化时由订阅回调整体重放 —— 不必再次遍历控件树，也不必保存控件成员。
// 表由窗口自身持有（成员），随窗口析构一起释放；动作只捕获本窗口的子控件
// 指针，因此不会出现"窗口已销毁仍被回调"的悬垂（订阅句柄先于子控件析构）。
// ---------------------------------------------------------------------------

using RepaintActions = std::vector<std::function<void()>>;

/// 重放全部登记动作（= 重新取 token 并写回控件）
inline void Replay(const RepaintActions& actions)
{
    for (const auto& action : actions) {
        if (action) {
            action();
        }
    }
}

/// 设置背景色并登记重放
inline void ApplyBackground(RepaintActions& actions, wxWindow* window, wxColour (*pick)())
{
    window->SetBackgroundColour(pick());
    actions.push_back([window, pick] { window->SetBackgroundColour(pick()); });
}

/// 设置前景色并登记重放
inline void ApplyForeground(RepaintActions& actions, wxWindow* window, wxColour (*pick)())
{
    window->SetForegroundColour(pick());
    actions.push_back([window, pick] { window->SetForegroundColour(pick()); });
}

// ---------------------------------------------------------------------------
// 字体（不变；DES-024 最小 10 DIP 已在 V6/F3 修正）
// ---------------------------------------------------------------------------

inline wxFont TitleFont(wxWindow* window)
{
    return wxFont(window->FromDIP(12), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                  wxFONTWEIGHT_BOLD);
}

// V6 / F3 修复（DES-024：全局最小字号 10 DIP，低于者必须上取整）：
// 原为 FromDIP(9)（9 DIP < 10 DIP 下限）→ 上取整到 10 DIP。
inline wxFont SectionFont(wxWindow* window)
{
    return wxFont(window->FromDIP(10), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                  wxFONTWEIGHT_BOLD);
}

inline wxFont SmallFont(wxWindow* window)
{
    return wxFont(window->FromDIP(10), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                  wxFONTWEIGHT_NORMAL);
}

// ---------------------------------------------------------------------------
// 控件样式 helper（均保留原签名 + 新增带主题重放的版本）
// ---------------------------------------------------------------------------

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
    StyleButton(button, kButton(), kText());
}

/// 带主题重放的按钮样式：立即取 token 上色，并登记"主题变化后重新取色"。
inline void StyleButton(RepaintActions& actions, wxButton* button,
                        wxColour (*background)(), wxColour (*foreground)())
{
    StyleButton(button, background(), foreground());
    actions.push_back([button, background, foreground] {
        StyleButton(button, background(), foreground());
    });
}

inline void StyleSecondaryButton(RepaintActions& actions, wxButton* button)
{
    StyleButton(actions, button, &kButton, &kText);
}

inline wxStaticText* MakeLabel(wxWindow* parent, const wxString& text,
                               const wxColour& colour = kText(), bool bold = false)
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

/// 带主题重放的标签（主题变化后前景色重新取 token）
inline wxStaticText* MakeLabel(RepaintActions& actions, wxWindow* parent,
                               const wxString& text, wxColour (*colour)(),
                               bool bold = false)
{
    wxStaticText* label = MakeLabel(parent, text, colour(), bold);
    actions.push_back([label, colour] { label->SetForegroundColour(colour()); });
    return label;
}

inline wxPanel* MakeDivider(wxWindow* parent, wxBoxSizer* layout, int margin = 0)
{
    wxPanel* divider = new wxPanel(parent);
    divider->SetBackgroundColour(kBorder());
    divider->SetMinSize(wxSize(-1, 1));
    layout->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);
    return divider;
}

/// 带主题重放的分隔线
inline wxPanel* MakeDivider(RepaintActions& actions, wxWindow* parent, wxBoxSizer* layout,
                            int margin = 0)
{
    wxPanel* divider = MakeDivider(parent, layout, margin);
    actions.push_back([divider] { divider->SetBackgroundColour(kBorder()); });
    return divider;
}

} // namespace FpgaTheme
