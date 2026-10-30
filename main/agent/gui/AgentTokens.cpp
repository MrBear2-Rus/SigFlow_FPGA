//
// AgentTokens.cpp — Agent 局部 Token 门面实现（AGUI-T04）
//
// 铁律：本文件**不得出现任何色值字面量**（hex / 三参色值构造 / 裸三元组）。
// 所有颜色都通过 UiTheme::AgentPalette() 的成员指针解析；所有尺寸都来自
// UiTheme::AgentMetrics / AgentSpacing / AgentTypography / AgentIcon …
// 逐项溯源见 GUI/agent-gui/phase1/token-map.md。
//

#include "agent/gui/AgentTokens.h"

#include "agent/gui/AgentLayoutPolicy.h"

#include <wx/dcclient.h>

#include <array>
#include <cmath>
#include <utility>

namespace agent::gui::tokens {
namespace {

namespace M = UiTheme::AgentMetrics;
namespace S = UiTheme::AgentSpacing;
namespace T = UiTheme::AgentTypography;
namespace I = UiTheme::AgentIcon;
namespace O = UiTheme::AgentOpacity;

// ===========================================================================
// 组件 Token 表
//
// 取值规则（每一行都能追到 token-map.md 的行）：
//   * radius/padding/gap/minHeight 直接取 AgentMetrics/AgentSpacing 的具名常量；
//   * fontSizePx/weight/lineHeightRatio 取 AgentTypography；
//   * 颜色用 AgentColours 的**成员指针**，不复制色值；
//   * 行高按 ratio 保存（DPI 只在 LineHeightDip 里乘一次，避免重复取整）。
// ===========================================================================

constexpr ComponentTokens kRail{
    /*radiusDip*/ M::ControlRadius,        // H315 .rb 圆角 r-sm
    /*paddingXDip*/ S::RailPadX,           // H311 padding:10px 6px
    /*paddingYDip*/ S::RailPadY,
    /*gapDip*/ S::RailGap,                 // H311 gap:6px
    /*minHeightDip*/ M::RailButton,        // H315 .rb 38×38
    /*minWidthDip*/ M::RailWidth,          // H311 width:58px
    /*maxWidthDip*/ M::RailWidth,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::SmallPx,             // rail 无可见文字；11px 为按钮级最小文字档(H250)
    /*weight*/ T::Medium,
    /*lineHeightRatio*/ T::LineHeightHint,
    /*iconDip*/ I::RailButton,             // H316 svg 18×18
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ 1.0,
    /*surface*/ &UiTheme::AgentColours::glassRailTop,   // H311 顶 .82
    /*text*/ &UiTheme::AgentColours::text2,             // H315 color:var(--text2)
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accentBorder,  // H318 accent .40
};

constexpr ComponentTokens kTopBar{
    /*radiusDip*/ 0,                       // H89 直角条；圆角在内部 mode-seg/status
    /*paddingXDip*/ S::TopBarPadX,         // H89 padding:0 16px
    /*paddingYDip*/ 0,                     // H89 min-height 对中心对齐，无上下内距
    /*gapDip*/ S::TopBarGap,               // H89 gap:14px
    /*minHeightDip*/ M::TitleBarHeight,    // H89 min-height:46px
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ 0,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::BrandPx,             // H94 .brand b 13.5px/600
    /*weight*/ T::Semibold,
    /*lineHeightRatio*/ T::LineHeightText,
    /*iconDip*/ I::WindowLight,            // H91 12px 窗口灯
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ O::RailBottom,             // H89 rgba(255,255,255,.62)
    /*surface*/ &UiTheme::AgentColours::glassRailBottom,
    /*text*/ &UiTheme::AgentColours::text1,
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accentSoft,    // H98 段控件选中 = 白底，见 token-map 备注
};

constexpr ComponentTokens kThread{
    /*radiusDip*/ 0,                       // H208 滚动区直角；气泡自带圆角
    /*paddingXDip*/ S::ThreadPadX,         // H208 max(16px, calc((100% - 760px)/2))
    /*paddingYDip*/ S::ThreadPadY,         // H208 padding:14px …
    /*gapDip*/ S::ThreadGap,               // H208 gap:13px
    /*minHeightDip*/ 0,
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ M::ThreadMaxWidth,     // H208 内容列 ≤760
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::BodyPx,              // H211 .msg 13px
    /*weight*/ T::Regular,
    /*lineHeightRatio*/ T::LineHeightBody, // H211 line-height:1.72
    /*iconDip*/ I::Evidence,               // H225 证据勾 11px
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ 1.0,
    /*surface*/ &UiTheme::AgentColours::cardGlass,      // H192 渐变玻璃（预合成）
    /*text*/ &UiTheme::AgentColours::text1,
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accent,
};

constexpr ComponentTokens kComposer{
    /*radiusDip*/ M::ControlRadius,        // H255 input r-sm=6
    /*paddingXDip*/ S::ComposePadX,        // H254 padding:11px 14px 13px
    /*paddingYDip*/ S::ComposePadY,
    /*gapDip*/ S::ComposeGap,              // H254 gap:8px
    /*minHeightDip*/ M::ComposerHeight,    // H255 height:40px
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ 0,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::BodyPx,              // H255 font-size:13px
    /*weight*/ T::Regular,
    /*lineHeightRatio*/ T::LineHeightText,
    /*iconDip*/ I::Composer,               // H259 16px
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,         // H257 3px 光晕另计（M::FocusGlow）
    /*mono*/ false,
    /*opacity*/ O::Input,                  // H255 rgba(255,255,255,.78)
    /*surface*/ &UiTheme::AgentColours::glassInput,
    /*text*/ &UiTheme::AgentColours::text1,
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accent,        // H257 focus 边 = accent
};

constexpr ComponentTokens kCard{
    /*radiusDip*/ M::CardRadius,           // H242/H229 r=10
    /*paddingXDip*/ S::CardHeadPadX,       // H243 padding:8px 12px / H245 10px 12px
    /*paddingYDip*/ S::CardBodyPadY,
    /*gapDip*/ S::CardBodyGap,             // H245 gap:8px
    /*minHeightDip*/ 0,
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ 0,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::SmallBodyPx,         // H243 .card-h 11.5px/600
    /*weight*/ T::Semibold,
    /*lineHeightRatio*/ T::LineHeightText,
    /*iconDip*/ I::Caret,                  // H231 12px chevron
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ O::Card,                   // H242 rgba(255,255,255,.66)
    /*surface*/ &UiTheme::AgentColours::glassCard,
    /*text*/ &UiTheme::AgentColours::text1,
    /*border*/ &UiTheme::AgentColours::glassEdgeHighlight, // H242 1px rgba(255,255,255,.85)
    /*emphasis*/ &UiTheme::AgentColours::accentSoft,
};

constexpr ComponentTokens kFloat{
    /*radiusDip*/ M::CardRadius,           // H270 复用 .aw r=10
    /*paddingXDip*/ M::FloatInset,         // H270 top/right:16px
    /*paddingYDip*/ M::FloatInset,
    /*gapDip*/ 0,                          // 浮层本身无 gap（内部 .aw 布局负责）
    /*minHeightDip*/ 0,
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ M::FloatMaxWidth,      // H270 min(400px, calc(100% - 32px))
    /*maxHeightDip*/ M::FloatMaxHeight,    // H270 min(596px, calc(100% - 32px))
    /*fontSizePx*/ T::BodyPx,
    /*weight*/ T::Regular,
    /*lineHeightRatio*/ T::LineHeightBody,
    /*iconDip*/ I::HeadTool,               // H202 .wt 15px
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ O::PaneTop,                // H192/H214 顶 .80
    /*surface*/ &UiTheme::AgentColours::glassPaneTop,
    /*text*/ &UiTheme::AgentColours::text1,
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accentSoft,
};

constexpr ComponentTokens kPet{
    /*radiusDip*/ M::CardRadius,           // H280/H286 r=10（左下角 3 见 BubbleTailRadius）
    /*paddingXDip*/ S::PetSayPadX,         // H280 padding:9px 12px
    /*paddingYDip*/ S::PetSayPadY,
    /*gapDip*/ S::PetSayMarginBottom,      // H280 margin-bottom:12px
    /*minHeightDip*/ 0,
    /*minWidthDip*/ M::PetWidth,           // H278 width:248px
    /*maxWidthDip*/ M::PetWidth,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::LabelPx,             // H280 .pet-say 12px
    /*weight*/ T::Regular,
    /*lineHeightRatio*/ T::LineHeightText, // H280 line-height:1.6
    /*iconDip*/ I::PetBadge,               // H285 13px
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,         // H287 focus-within 3px 光晕
    /*mono*/ false,
    /*opacity*/ O::GlassStrong,            // H280/H286 --glass-2 .84
    /*surface*/ &UiTheme::AgentColours::cardGlassStrong,
    /*text*/ &UiTheme::AgentColours::text1,
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accent,
};

constexpr ComponentTokens kBadge{
    /*radiusDip*/ M::PillRadius,           // H100/H199/H141 pill
    /*paddingXDip*/ S::TagPadX,            // H118/H199 padding:2px 8px
    /*paddingYDip*/ S::TagPadY,
    /*gapDip*/ S::TagGap,                  // H100/H199 圆点与文字间距
    /*minHeightDip*/ 0,
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ 0,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::TinyPx,              // H100 11px/600、H199 10.5px/600
    /*weight*/ T::Semibold,
    /*lineHeightRatio*/ T::LineHeightHint,
    /*iconDip*/ I::DotSmall,               // H142/H164 6px 圆点
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ 1.0,
    /*surface*/ &UiTheme::AgentColours::accentSoft,     // H141/H199 蓝浅底
    /*text*/ &UiTheme::AgentColours::accentText,        // H141/H199 深蓝字
    // 蓝徽章边 = accent .30（H141 wf-chip）。.demo-badge 的 danger .28 走
    // BorderRole::DangerWeak（H100），不在这里混用同一档，见 token-map §3。
    /*border*/ &UiTheme::AgentColours::accentBorderSoft,
    /*emphasis*/ &UiTheme::AgentColours::accentBorder,  // 选中/强调用 .40
};

constexpr ComponentTokens kButton{
    /*radiusDip*/ M::ControlRadius,        // H201/H250/H315 r-sm=6
    /*paddingXDip*/ S::LevelPadX,          // H250 padding:4px 9px（.level）
    /*paddingYDip*/ S::LevelPadY,
    /*gapDip*/ S::LevelGap,                // H324 .lv-seg gap:3px
    /*minHeightDip*/ M::ButtonHeight,      // H315 .rb 38（窄屏 34 = RailButtonNarrow）
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ 0,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::SmallPx,             // H250 .level 11px/500
    /*weight*/ T::Medium,
    /*lineHeightRatio*/ T::LineHeightHint,
    /*iconDip*/ I::HeadTool,               // H202 .wt 15px
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ O::DisabledAction,         // H181 disabled .65（实心态 1.0）
    /*surface*/ &UiTheme::AgentColours::cardBg,         // H250 background:var(--panel)
    /*text*/ &UiTheme::AgentColours::text2,
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accentSoft,    // H252 选中底
};

constexpr ComponentTokens kPill{
    /*radiusDip*/ M::PillRadius,           // H159/H184/H188/H227 pill
    /*paddingXDip*/ S::PillPadX,           // H159 padding:3px 9px
    /*paddingYDip*/ S::PillPadY,
    /*gapDip*/ S::PillGap,                 // H159 gap:5px
    /*minHeightDip*/ 0,
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ 0,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::TinyPx,              // H159 10.5px/500
    /*weight*/ T::Medium,
    /*lineHeightRatio*/ T::LineHeightHint,
    /*iconDip*/ I::DotSmall,               // H164 6px
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ 1.0,
    /*surface*/ &UiTheme::AgentColours::cardBg,         // H159 background:var(--panel)
    /*text*/ &UiTheme::AgentColours::text2,
    // 中性胶囊用 hairline；语义胶囊按状态取 BorderRole：
    //   red→Danger(.32 H160)、blue→AccentMid(.35 H161)、green→Ok(.32 H162)、
    //   amber→Warn(.32 H163)、chk.warn→WarnWeak(.30 H186)。
    // **不提供**"一个中间值代表全部"的合并 token（原型 alpha 是散值）。
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accentSoft,    // H161 .pill.blue 底
};

constexpr ComponentTokens kEvidenceRow{
    /*radiusDip*/ M::ControlRadius,        // H174 .cite li r-sm
    /*paddingXDip*/ S::CitePadX,           // H174 padding:7px 9px
    /*paddingYDip*/ S::CitePadY,
    /*gapDip*/ S::EvidenceGapY,            // H223 gap:6px 12px
    /*minHeightDip*/ 0,
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ 0,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::SmallBodyPx,         // H168/H175 11.5px/500
    /*weight*/ T::Medium,
    /*lineHeightRatio*/ T::LineHeightText,
    /*iconDip*/ I::Evidence,               // H225 11px 绿勾
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ false,
    /*opacity*/ 1.0,
    /*surface*/ &UiTheme::AgentColours::cardGlass,      // H174 卡行沿用玻璃
    /*text*/ &UiTheme::AgentColours::text2,
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::brand,         // H225 stroke:var(--green)
};

constexpr ComponentTokens kCodeBlock{
    /*radiusDip*/ M::ControlRadius,        // 派生档：沿用控件圆角
    /*paddingXDip*/ S::CardBodyPadX,       // 派生档：与卡片正文同内距
    /*paddingYDip*/ S::CardBodyPadY,
    /*gapDip*/ S::CardBodyGap,
    /*minHeightDip*/ 0,
    /*minWidthDip*/ 0,
    /*maxWidthDip*/ 0,
    /*maxHeightDip*/ 0,
    /*fontSizePx*/ T::MicroPx,             // H177 .pid font-size:10px（唯一等宽用例）
    /*weight*/ T::Regular,
    /*lineHeightRatio*/ T::LineHeightHint,
    /*iconDip*/ 0,                         // 代码块无图标
    /*borderDip*/ M::Hairline,
    /*focusRingDip*/ M::FocusRing,
    /*mono*/ true,                         // H177 font-family:var(--mono)
    /*opacity*/ O::Card,
    /*surface*/ &UiTheme::AgentColours::cardGlass,
    /*text*/ &UiTheme::AgentColours::accentText,        // H177 color:var(--accent-text)
    /*border*/ &UiTheme::AgentColours::hairline,
    /*emphasis*/ &UiTheme::AgentColours::accentSoft,
};

constexpr std::array<ComponentTokens, 12> kTokens{kRail,   kTopBar, kThread, kComposer,
                                                  kCard,   kFloat,  kPet,    kBadge,
                                                  kButton, kPill,   kEvidenceRow, kCodeBlock};

// --- 编译期保证：表的行数与枚举一致，且每行的关键量都成立（不做机械逐项测试）---
static_assert(kTokens.size() == 12, "Component 枚举与 Token 表行数必须一致");
static_assert(kTokens[static_cast<std::size_t>(Component::Rail)].minWidthDip > 0,
              "rail 必须有确定宽度 58 DIP");
static_assert(kTokens[static_cast<std::size_t>(Component::Thread)].maxWidthDip > 0,
              "对话列必须有最大宽度（≤760 DIP）");
static_assert(kTokens[static_cast<std::size_t>(Component::Float)].maxWidthDip > 0 &&
                  kTokens[static_cast<std::size_t>(Component::Float)].maxHeightDip > 0,
              "浮窗必须有 min(400,…)/min(596,…) 上界");
static_assert(kTokens[static_cast<std::size_t>(Component::Composer)].minHeightDip > 0,
              "输入行必须有 40 DIP 高度");
static_assert(kTokens[static_cast<std::size_t>(Component::Pill)].radiusDip >
                  kTokens[static_cast<std::size_t>(Component::Pill)].minHeightDip,
              "胶囊圆角必须大到可被裁成半圆");
static_assert(kTokens[static_cast<std::size_t>(Component::CodeBlock)].mono,
              "代码块必须用等宽族");

template <std::size_t N>
constexpr bool RowsAreSane(const std::array<ComponentTokens, N>& rows)
{
    for (const ComponentTokens& row : rows) {
        if (row.paddingXDip < 0 || row.paddingYDip < 0 || row.gapDip < 0) return false;
        if (row.fontSizePx <= 0.0 || row.lineHeightRatio <= 1.0) return false;
        if (row.borderDip <= 0 || row.focusRingDip <= 0) return false;
        if (row.radiusDip < 0 || row.iconDip < 0) return false;
        if (row.surface == nullptr || row.text == nullptr || row.border == nullptr ||
            row.emphasis == nullptr) {
            return false;
        }
    }
    return true;
}
static_assert(RowsAreSane(kTokens), "任一组件 Token 行都必须有正字号、有效边线与四个颜色角色");

/// 缩放比例：**从 wx 自己的 FromDIP 反推**，而不是用 GetDPIScaleFactor()。
///
/// 依据（本地 wx 3.2 源码 3rd/wxWidgets-src/src/common/wincmn.cpp:2958-2965）：
///     wxWindowBase::FromDIP(const wxSize& sz, const wxWindowBase* w)
///         const wxSize dpi      = GetDPIHelper(w);          // w->GetDPI() → screen DC PPI → 标准 PPI
///         const wxSize baseline = wxDisplay::GetStdPPI();   // 通常 96
///         return wxRescaleCoord(sz).From(baseline).To(dpi); // 内部 wxMulDivInt32（MulDiv 四舍五入）
/// 也就是说 FromDIP 用的是 **PPI 比值**（dpi / stdPPI），而 GetDPIScaleFactor() 是
/// 显示器的 scale factor —— 两者不保证相等（例如端口上报 109 PPI 时比值是 109/96≈1.135，
/// 而 scale factor 可能是 1.25）。若 double 版用前者、int 版用后者，同一逻辑单位会出现
/// 两套结果。这里统一取 `FromDIP(100)/100`：100 足够大，把整数取整误差压到 1e-2 以内，
/// 且与 FromDIP(int) 逐条一致（同一 MulDiv 口径）。
double ScaleFactorOf(const wxWindow* window)
{
    const int hundredDipPx =
        window != nullptr ? window->FromDIP(100) : wxWindow::FromDIP(100, nullptr);
    if (hundredDipPx > 0) {
        return static_cast<double>(hundredDipPx) / 100.0;
    }
    return 1.0;  // 端口拿不到 PPI 时按 96 DPI 语义（1 DIP = 1 px）
}

/// 公共 API 的有界性常量（见头文件"有界性"说明）
constexpr double kMaxCssPx = 1.0e6;  ///< 1e6 CSS px ≈ 26000pt，远超任何 UI 尺寸

/// 把任意 double 规整成 [0, kMaxCssPx]：NaN/±Inf/负数 → 0。
double BoundCssPx(double cssPx)
{
    if (!std::isfinite(cssPx) || cssPx <= 0.0) {
        return 0.0;
    }
    return cssPx > kMaxCssPx ? kMaxCssPx : cssPx;
}

}  // namespace

// ===========================================================================
// 调色板
// ===========================================================================
const UiTheme::AgentColours& Colours()
{
    return UiTheme::AgentPalette();
}

const UiTheme::AgentColours& ColoursFor(UiTheme::Mode mode)
{
    return UiTheme::AgentColoursFor(mode);
}

const ComponentTokens& Tokens(Component component)
{
    const auto index = static_cast<std::size_t>(component);
    if (index >= kTokens.size()) {
        return kTokens.front();  // 枚举越界（不应发生）：退回 Rail，不抛异常
    }
    return kTokens[index];
}

const char* ComponentToken(Component component)
{
    switch (component) {
        case Component::Rail: return "rail";
        case Component::TopBar: return "topbar";
        case Component::Thread: return "thread";
        case Component::Composer: return "composer";
        case Component::Card: return "card";
        case Component::Float: return "float";
        case Component::Pet: return "pet";
        case Component::Badge: return "badge";
        case Component::Button: return "button";
        case Component::Pill: return "pill";
        case Component::EvidenceRow: return "evidence-row";
        case Component::CodeBlock: return "code-block";
    }
    return "rail";
}

wxColour SurfaceColour(Component component)
{
    const ComponentTokens& tokens = Tokens(component);
    const UiTheme::AgentColours& palette = Colours();
    return tokens.surface != nullptr ? palette.*tokens.surface : palette.cardBg;
}

wxColour TextColour(Component component)
{
    const ComponentTokens& tokens = Tokens(component);
    const UiTheme::AgentColours& palette = Colours();
    return tokens.text != nullptr ? palette.*tokens.text : palette.text1;
}

wxColour BorderColour(Component component)
{
    const ComponentTokens& tokens = Tokens(component);
    const UiTheme::AgentColours& palette = Colours();
    return tokens.border != nullptr ? palette.*tokens.border : palette.hairline;
}

wxColour EmphasisColour(Component component)
{
    const ComponentTokens& tokens = Tokens(component);
    const UiTheme::AgentColours& palette = Colours();
    return tokens.emphasis != nullptr ? palette.*tokens.emphasis : palette.accent;
}

wxColour BorderColour(BorderRole role)
{
    const UiTheme::AgentColours& palette = Colours();
    switch (role) {
        case BorderRole::Hairline: return palette.hairline;              // H49
        case BorderRole::AccentSoft: return palette.accentBorderSoft;    // .30 H141
        case BorderRole::AccentMid: return palette.borderAccentMid;      // .35 H161
        case BorderRole::Accent: return palette.accentBorder;            // .40 H126/H318
        case BorderRole::AccentStrong: return palette.borderAccentStrong;// .45 H236
        case BorderRole::DangerWeak: return palette.borderDangerWeak;    // .28 H100
        case BorderRole::Danger: return palette.dangerBorder;            // .32 H160
        case BorderRole::Ok: return palette.okBorder;                    // .32 H162
        case BorderRole::WarnWeak: return palette.borderWarnWeak;        // .30 H186
        case BorderRole::Warn: return palette.warnBorder;                // .32 H163
        case BorderRole::WarnMid: return palette.borderWarnMid;          // .35 H128
        case BorderRole::MutedDashed: return palette.mutedBorder;        // .35 H229
    }
    return palette.hairline;
}

// ===========================================================================
// 单位换算
// ===========================================================================
int CssPxToDip(const wxWindow* window, int cssPx)
{
    // 整数入参交给 wx：FromDIP 内部是 MulDiv(n, dpi, stdPPI) 四舍五入
    // （wincmn.cpp:2958），与既有的 UiTheme::UiFont 口径一致（PLT-001）。
    // 0/负数按 0 处理：尺寸不应把负值带进 Sizer（那会被 wx 当成默认值/异常值）。
    if (cssPx <= 0) {
        return 0;
    }
    if (window != nullptr) {
        return window->FromDIP(cssPx);
    }
    return wxWindow::FromDIP(cssPx, nullptr);
}

int CssPxToDip(const wxWindow* window, double cssPx)
{
    // 分数入参：缩放比例从 FromDIP 反推（见 ScaleFactorOf 的注释），再 lround。
    // lround 是"四舍五入、.5 远离 0"：10.5 → 11、11.5 → 12、12.5 → 13、13.5 → 14。
    // 取整到上一档是**刻意**的：宁可略大，也不要把 10.5 压成 10（小号中文在
    // GDI 下 10px 已接近可读边界）。100% 缩放因此分数档与上一整数档同高，
    // 该 ±0.5px 差异见 token-map「原生无法精确表达」。
    //
    // 有界性（公共 API 契约，见头文件）：NaN/±Inf/负数 → 0；> 1e6 CSS px → 1e6。
    // 这样 lround 的输入恒在 [0, 1e6×scale≈4e6] < INT_MAX，不会触发 UB/溢出。
    const double boundedCssPx = BoundCssPx(cssPx);
    if (boundedCssPx <= 0.0) {
        return 0;
    }
    const double scaled = boundedCssPx * ScaleFactorOf(window);
    if (!std::isfinite(scaled) || scaled <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::lround(scaled));
}

double CssPxToPoint(double cssPx)
{
    // **恒** 0.75：CSS px 是逻辑像素（1/96 in），pt 是 1/72 in ⇒ 72/96 = 0.75，
    // 与显示器 DPI/缩放无关。12px ≡ 9pt，13px ≡ 9.75pt。
    // 旧实现写成 cssPx*72/dpi 是把逻辑 px 当物理 px（120 DPI 下把 12px 算成 7.2pt），已修正。
    const double boundedCssPx = BoundCssPx(cssPx);
    return boundedCssPx * 72.0 / 96.0;
}

double PhysicalPxToPoint(double physicalPx, int dpi)
{
    // 只用于**物理像素**（GetDPI/GetPPI 的返回值）：pt = px × 72 / dpi。
    const double boundedPx = BoundCssPx(physicalPx);
    const int effectiveDpi = dpi > 0 ? dpi : 96;
    return boundedPx * 72.0 / static_cast<double>(effectiveDpi);
}

// ===========================================================================
// 字体
// ===========================================================================
wxFont UiFontPx(const wxWindow* window, double cssPx, Weight weight)
{
    const int pixelHeight = CssPxToDip(window, cssPx);
    // 先取 UiTheme 的系统 GUI face + 字重口径（THE-016：只有 Family 会留下空 face，
    // GDI+ 会用错字体），再把高度换成小数档取整后的像素高度。
    wxFont font = UiTheme::UiFont(window, UiTheme::TextSize::P13, weight);
    font.SetPixelSize(wxSize(0, pixelHeight > 0 ? pixelHeight : 1));
    return font;
}

wxFont MonoFontPx(const wxWindow* window, double cssPx, Weight weight)
{
    const int pixelHeight = CssPxToDip(window, cssPx);
    wxFont font = UiTheme::MonoFont(window, UiTheme::TextSize::P13, weight);
    font.SetPixelSize(wxSize(0, pixelHeight > 0 ? pixelHeight : 1));
    return font;
}

int LineHeightDip(const wxWindow* window, double fontSizePx, double ratio)
{
    if (fontSizePx <= 0.0) {
        return 0;
    }
    const double effectiveRatio = ratio > 0.0 ? ratio : 1.0;
    // 行高按"整行高度"一次取整：fontSizePx × ratio 先在 CSS px 域算完，
    // 再乘缩放比例（CssPxToDip 内部），避免"先取整字号再乘倍率"与
    // "字号 + 行距分别取整"两套口径打架（13×1.72 = 22.36 → 22）。
    return CssPxToDip(window, fontSizePx * effectiveRatio);
}

int LineHeightDip(const wxWindow* window, const ComponentTokens& tokens)
{
    return LineHeightDip(window, tokens.fontSizePx, tokens.lineHeightRatio);
}

// ===========================================================================
// 主题订阅
// ===========================================================================
UiTheme::Subscription SubscribeTheme(std::function<void()> callback)
{
    // 透传：句柄析构即解绑（THE-028）。Agent 组件不得自建订阅表。
    return UiTheme::Subscribe(std::move(callback));
}

// ===========================================================================
// AGUI-T11：物理像素 → DIP（断点判定口径）+ 布局策略输入适配
// ===========================================================================
int DipToCssPx(const wxWindow* window, int physicalPx)
{
    if (physicalPx <= 0) {
        return 0;
    }
    const double scale = ScaleFactorOf(window);
    if (!std::isfinite(scale) || scale <= 0.0) {
        return physicalPx;  // 无窗口/异常缩放：按 1:1 处理，不猜
    }
    const double boundedPx = std::min(static_cast<double>(physicalPx), 1e6);
    const double css = boundedPx / scale;
    if (!std::isfinite(css) || css <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::lround(css));
}

layout::Metrics LayoutMetrics()
{
    layout::Metrics metrics;
    metrics.wideMax = M::WideBreakpoint;
    metrics.mediumMax = M::MediumBreakpoint;
    metrics.narrowMax = M::NarrowBreakpoint;

    metrics.guideWide = M::ColumnLeftWidth;
    metrics.guideCompact = M::ColumnLeftCompact;
    metrics.supportWide = M::ColumnRightWidth;
    metrics.supportCompact = M::ColumnRightCompact;
    metrics.pagePad = S::PagePadding;
    metrics.columnGap = S::ColumnGap;
    metrics.mainMinWidth = M::MainMinWidth;
    metrics.mainMinHeight = M::MainMinHeight;
    metrics.sideMinHeight = M::SideMinHeight;

    metrics.railWidth = M::RailWidth;
    metrics.railButton = M::RailButton;
    metrics.railButtonNarrow = M::RailButtonNarrow;
    metrics.railLogo = M::RailLogoSize;
    metrics.railLogoNarrow = M::RailLogoSizeNarrow;

    metrics.threadMaxWidth = M::ThreadMaxWidth;
    metrics.threadPadMin = S::ThreadPadX;

    metrics.floatInsetWide = M::FloatInset;
    metrics.floatInsetCompact = M::FloatInsetCompact;
    metrics.floatMaxWidth = M::FloatMaxWidth;
    metrics.floatMaxHeight = M::FloatMaxHeight;
    metrics.floatMaxHeightCompact = M::FloatMaxHeightCompact;

    metrics.petWidthWide = M::PetWidth;
    metrics.petWidthNarrow = M::PetWidthNarrow;
    metrics.petHeightWide = M::PetHeight;
    metrics.petHeightNarrow = M::PetHeightNarrow;
    metrics.petFigureWide = M::PetFigureSize;
    metrics.petFigureNarrow = M::PetFigureSizeNarrow;
    metrics.petInsetWide = S::PetBottom;
    metrics.petInsetCompact = S::PetCompactInset;
    metrics.petLeftWide = S::PetLeft;
    metrics.petClamp = M::FloatMargin;
    return metrics;
}

// ===========================================================================
// 文本折行
// ===========================================================================

WrappedText WrapTextForWidth(wxWindow* owner, const wxFont& font, const wxString& source,
                             int width)
{
    WrappedText result;
    if (owner == nullptr) return result;
    wxClientDC dc(owner);
    dc.SetFont(font);
    const int limit = std::max(1, width);
    wxString line;
    for (auto ch : source) {
        if (ch == '\n') {
            result.text += line + "\n";
            line.clear();
            continue;
        }
        wxString candidate = line;
        candidate += ch;
        if (!line.empty() && dc.GetTextExtent(candidate).x > limit) {
            result.text += line + "\n";
            line.clear();
        }
        line += ch;
    }
    result.text += line;
    wxCoord measuredWidth = 0;
    wxCoord measuredHeight = 0;
    dc.GetMultiLineTextExtent(result.text, &measuredWidth, &measuredHeight);
    result.height = std::max(0, static_cast<int>(measuredHeight));
    return result;
}

}  // namespace agent::gui::tokens
