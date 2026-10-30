#pragma once
//
// UiTheme — SigFlow 唯一设计 Token 层（T-110）
//
// 规范依据：docs/GUI-CHUGAO/SigFlow-界面改造SPEC.md v2.0
//   §3.3 基础族调色板（唯一视觉权威 = GUI/app.html，2026-10-10 冻结）
//   §3.3 末表 扩展族（教学 / Agent 状态语义色；来源 docs/gui/SigFlow-MainFrame-Upgrade-Spec.md:123-127）
//   §3.4 尺度 / 字号 / 字重 / 动效档
//   §3.5 合成实色（wx 控件底色不参与父层 alpha 合成，故按已知底色预合成）
//   §14.1 DEC-02 裁决：设计稿表为唯一基础族 + 教学语义色并入同一模块的扩展族
//
// 红线：
//   SAFE-012 —— 本轮触达的文件不得新增硬编码颜色，一律经本模块取用
//   THE-001  —— 全工程只允许这一套 Token 层
//   THE-002  —— 语义名（如 bgWindow / text1 / accent）确定后不得改名
//   PLT-001  —— 尺寸一律 DIP（本模块内所有像素值经 wxWindow::FromDIP 换算）
//   PLT-002  —— 字体族取系统 GUI 字体 / wxFONTFAMILY_TELETYPE，禁止写死具体族名
//
// 用法示例：
//     auto bg = UiTheme::Palette().bgPanel;
//     panel->SetBackgroundColour(UiTheme::Palette().surfaceOnPanel);
//     label->SetFont(UiTheme::UiFont(this, UiTheme::TextSize::P12, UiTheme::Weight::Medium));
//     m_themeSub = UiTheme::Subscribe([this] { ApplyTheme(); });
//

#include <wx/colour.h>
#include <wx/font.h>
#include <wx/string.h>
#include <wx/window.h>

#include <functional>
#include <utility>

namespace UiTheme {

// ---------------------------------------------------------------------------
// 主题档位（THE-006）
// ---------------------------------------------------------------------------
enum class Mode
{
    System,  ///< 跟随系统；无法探测时回退 Light
    Light,
    Dark
};

// ---------------------------------------------------------------------------
// 字号档（THE-017：DIP 整数档）
// ---------------------------------------------------------------------------
enum class TextSize : int
{
    P10 = 10,
    P11 = 11,
    P12 = 12,
    P13 = 13,
    P14 = 14,
    P16 = 16,
    P19 = 19
};

// 字重档（THE-018）
enum class Weight : int
{
    Normal = 400,
    Medium = 500,
    Semibold = 600,
    Bold = 700
};

// ---------------------------------------------------------------------------
// 尺度档（THE-011 圆角 / THE-012 间距）
// ---------------------------------------------------------------------------
namespace Radius {
inline constexpr int XS        = 6;   ///< 控件
inline constexpr int SM        = 9;   ///< 次级
inline constexpr int MD        = 13;  ///< 卡片 / 浮层
inline constexpr int LG        = 18;  ///< 结构面（底栏 / 面板容器）
inline constexpr int XL        = 24;  ///< 窗口
inline constexpr int Card      = 12;  ///< 画布元件卡（§5.1 局部规格）
inline constexpr int Indicator = 2;   ///< 页签指示条厚度
inline constexpr int RailMark  = 3;   ///< 活动栏选中指示条宽
inline constexpr int RailMarkL = 18;  ///< 活动栏选中指示条高（CMP-028）
}  // namespace Radius

namespace Spacing {
inline constexpr int S1       = 4;
inline constexpr int S2       = 8;
inline constexpr int S3       = 12;
inline constexpr int S4       = 16;
inline constexpr int S5       = 22;
inline constexpr int S6       = 30;
inline constexpr int Baseline = 8;
}  // namespace Spacing

// 区域/表单的几何预算（RV-T01）。这些值仍以 DIP 表达，供宿主和原生
// 子控件共同消费，避免各个 builder 在局部重新发明间距和最小高度。
namespace RegionMetrics {
inline constexpr int ContentInset = 8;   ///< 圆角宿主内容安全内缩
inline constexpr int FieldHeight  = 28;  ///< 原生输入/下拉最小高度
inline constexpr int FieldRadius  = 12;  ///< 输入壳圆角（加大到 12，更接近设计稿）
inline constexpr int FieldPadX    = 8;   ///< 输入文字与壳边缘的水平余量
inline constexpr int FieldGap     = 8;   ///< 表单行之间的紧凑间距
inline constexpr int CardGap      = 12;  ///< 面板卡片之间的常规间距
inline constexpr int ShadowReserve = 4;  ///< 宿主轻影预留
}  // namespace RegionMetrics

// 动效时长（THE-014，单位 ms）
namespace Motion {
inline constexpr int Press = 140;
inline constexpr int Quick = 180;
inline constexpr int Panel = 260;
}  // namespace Motion

// 模糊尺度（THE-015，**仅作为层级权重注记**；wx 侧不得调用真实模糊，SAFE-008）
namespace Blur {
inline constexpr int SM = 16;
inline constexpr int MD = 28;
inline constexpr int LG = 44;
}  // namespace Blur

// 骨架尺寸（LAY-002 / LAY-006 / LAY-009，单位 DIP）
namespace Skeleton {
inline constexpr int TitleBarH = 38;
inline constexpr int MenuBarH  = 32;
inline constexpr int ToolBarH  = 48;
inline constexpr int RailW     = 56;
inline constexpr int LeftW     = 282;
inline constexpr int LeftMinW  = 210;
inline constexpr int LeftMaxW  = 340;
inline constexpr int RightW    = 264;  // DEC-04 裁决
inline constexpr int RightMinW = 210;
inline constexpr int RightMaxW = 520;
inline constexpr int BottomH   = 174;
inline constexpr int StatusBarH = 26;  ///< 状态栏**目标**高度（DIP）。⚠️ 本仓**刻意不施加**：
                                       ///< LAY-012 是 SHOULD 且原文允许"若平台不接受定制高度，
                                       ///< 允许原生高度（PLT-004）"，验收判据仅为"**记录实际高度**"。
                                       ///< 实测（T-720，150% DPI）：原生 `wxStatusBar` = 32px = **21 DIP**，
                                       ///< 不裁切。保留常量供参考 / 未来平台使用（F-720-02）。
inline constexpr int SplitMinPx = 50;
inline constexpr double SplitRatio = 0.62;  ///< 中区画布占比（LAY-010：画布 62% / 编辑器 38%）
inline constexpr int ToolBarChipH = 30;     ///< 顶部工具栏 chip 高度（CMP-021）
inline constexpr int ChipPadX     = 10;     ///< chip 左右内距（设计稿 .chip `padding:0 10px`，app.html:254-266）
inline constexpr int RailKeySize  = 40;     ///< 活动栏按键 40×40（正方形；LAY-003 / CMP-032）
inline constexpr int RailKeyGap   = 10;     ///< 活动栏相邻按键之间的垂直间距（图标之间的高度差）
inline constexpr int ToolIconSize = 14;     ///< 工具栏图标基准 14 DIP（DES-014 chip 档）
}  // namespace Skeleton

// ---------------------------------------------------------------------------
// 调色板：基础族（SPEC §3.3）
// ---------------------------------------------------------------------------
struct Colours
{
    // ---- 底色 ----
    wxColour bgWindow;  ///< --hue-bg-1
    wxColour bgPanel;   ///< --hue-bg-2
    wxColour bgDeep;    ///< --hue-bg-3

    // ---- 面板 / 交互面（保留原始 alpha 值，仅供自绘 alpha 路径使用，THE-021）----
    wxColour surface;
    wxColour surfaceStrong;
    wxColour surfaceSunken;

    // ---- 描边与高光 ----
    wxColour stroke;
    wxColour strokeStrong;
    wxColour specular;

    // ---- 文字 ----
    wxColour text1;
    wxColour text2;
    wxColour text3;

    // ---- 画布 ----
    wxColour canvas;
    wxColour grid;
    wxColour gridStrong;
    wxColour wire;
    wxColour canvasShadow; ///< 元件卡软影，16% alpha；仅用于画布，不是真实 blur。

    // ---- 语义色 ----
    wxColour brand;   ///< 品牌绿：CTA / 幽灵放置 / 状态灯 / clk
    wxColour accent;  ///< 行动蓝：选中 / 焦点 / 链接 / rst
    wxColour warn;    ///< 警告 / 未保存 / bus
    wxColour danger;  ///< 错误 / 关闭键 hover

    // ---- 状态底 ----
    wxColour sel;
    wxColour hover;

    // ---- 终端（SPEC §3.5 补充表：刻意不随浅色档变化）----
    wxColour termBg;

    // ---- 合成实色（SPEC §3.5，来源 = 工程适配建议值；控件底色必须用这些）----
    wxColour surfaceOnWindow;    ///< surface 合成于 bgWindow
    wxColour surfaceOnPanel;     ///< surface 合成于 bgPanel
    wxColour surfaceOnCanvas;    ///< surface 合成于 canvas
    wxColour strongOnPanel;      ///< surfaceStrong 合成于 bgPanel（≈ #FEFEFE / #24252A）
    wxColour sunkenOnPanel;      ///< surfaceSunken 合成于 bgPanel
    wxColour sunkenOnCanvas;     ///< surfaceSunken 合成于 canvas
    wxColour strokeOnWindow;
    wxColour strokeOnPanel;
    wxColour strokeStrongOnWindow;
    wxColour strokeStrongOnPanel;
    wxColour selOnPanel;
    wxColour hoverOnPanel;
    wxColour focusRingOnPanel;   ///< accent 45% 合成于 bgPanel（THE-026）

    // GUI/app.html --glass-*；仅供 Synthesize CTA，禁止其他按键使用玻璃材质。
    wxColour glassEdge, glassEdgeStrong, glassEdgeSoft, glassRim;
    wxColour glassRing, glassShadow, glassBorder;

    // ---- 代码专用派生 token（SPEC §3.6，来源 = 工程适配建议值）----
    // 背景：编辑器的「类型」用 brand(浅色 4.38:1)、「注释」用 text3(浅色 3.36:1) 都低于
    // THE-023 的正文 4.5:1。按 DES-001「可读可对比优先」，在同色相内派生达到 4.5:1 的值，
    // 仅用于代码区，不改变 chrome 上的 token 取值。
    wxColour codeType;     ///< 浅 #1f864c(4.59:1) / 深 #3ed37c(9.40:1，与 brand 同值)
    wxColour codeComment;  ///< 浅 #71767f(4.56:1) / 深 #7c8290(4.74:1)
};

// ---------------------------------------------------------------------------
// 调色板：扩展族（DEC-02 教学 / Agent 状态语义色）
// 使用边界（MUST）：**仅允许**用于 Agent / 教学面板内部；主窗口 chrome 只能用基础族。
// ---------------------------------------------------------------------------
struct TeachingColours
{
    wxColour guideBg, guideText;
    wxColour doneBg, doneText;
    wxColour hardwareBg, hardwareText;
    wxColour errorBg, errorText;
    wxColour stateOk, stateWarn, stateBad;
};

// ---------------------------------------------------------------------------
// Agent 局部扩展（DEC-13 / AGUI-T02）
//
// 使用边界（MUST）：仅 Agent / 教学面板内部；主窗口 chrome 仍只用基础族。
// 本结构是**唯一** Agent 语义色定义处（THE-001）；组件/布局代码不得自建第二套
// 调色板，也不得写死色值（SAFE-012）。取值来源：GUI/agent-gui/index.html §1 的
// CSS 变量（bg/panel/hairline/text/accent/语义色/用户气泡渐变/CTA 渐变）。
// 说明：wx 侧不做真实模糊（SAFE-008）；玻璃色是**预合成**实色（cardGlass*），
// 不是 backdrop-filter 的还原。深色档仅按既有深色调色板推导，未做视觉对照（DEC-11）。
// ---------------------------------------------------------------------------
struct AgentColours
{
    wxColour pageBg;           ///< #FAFAF8 页面底
    wxColour pageTint;         ///< #EFF4FA 页底渐变 55% 停靠
    wxColour pageTint2;        ///< #F3F6F9 页底渐变末端
    wxColour cardBg;           ///< #FFFFFF 面板
    wxColour cardGlass;        ///< 面板白 70% 预合成于 pageBg（SAFE-008：不真模糊）
    wxColour cardGlassStrong;  ///< 面板白 84% 预合成（浮层 / 输入壳）
    wxColour hairline;         ///< #E5E5E5 1px 边线
    wxColour text1;            ///< #1D1D1F
    wxColour text2;            ///< #5F5E5A
    wxColour text3;            ///< #6E6D67
    wxColour accent;           ///< #007AFF
    wxColour accentText;       ///< #0C447C
    wxColour accentSoft;       ///< #E6F1FB
    wxColour brand;            ///< #26813D 绿色连接带 / 完成态
    wxColour okSoft;           ///< #EAF3DE
    wxColour warn;             ///< #A35A00
    wxColour warnSoft;         ///< #FAEEDA（演示标识 / 警告底）
    wxColour danger;           ///< #C73838
    wxColour dangerSoft;       ///< #FCEBEB
    wxColour userBubbleTop;    ///< 用户气泡渐变起点 #0A72DC（135°）
    wxColour userBubbleBottom; ///< 用户气泡渐变终点 #0053B4
    wxColour textOnAccent;     ///< 压在 accent 形底/用户气泡上的文字（浅深两档均为白）
    wxColour ctaTop;           ///< 主渐变起点 #3D95FF
    wxColour ctaBottom;        ///< 主渐变终点 #007AFF
    wxColour shadow;           ///< 浮层阴影近似色（预合成，非真实 blur）

    // ---- AGUI-T04 追加：逐组件语义色（设计源 GUI/agent-gui/index.html，行号见
    //      GUI/agent-gui/phase1/token-map.md）。以下全部是**预合成实色**：原型用
    //      rgba()/backdrop-filter 叠在玻璃上，wx 侧不做真实模糊（SAFE-008），
    //      原始 alpha 只作为权重记录在 UiTheme::AgentOpacity 中。----
    wxColour sunken;             ///< H48  #F1EFE8 下陷面（分段控件底 / hover）
    wxColour sunkenSoft;         ///< H205 rgba(241,239,232,.60) 预合成（对话状态条）
    wxColour sunkenSoftStrong;   ///< H243 rgba(241,239,232,.65) 预合成（卡片头）
    wxColour dangerText;         ///< H57  #791F1F 红字（演示徽章 / issue 摘要）
    wxColour okText;             ///< H60  #27500A 绿字（已完成 / 独立修复）
    wxColour warnText;           ///< H63  #712B13 琥珀字（需复测 / 建议复习）
    wxColour accentBorder;       ///< H126/H161/H189/H204/H252/H318 accent 40% 预合成
    wxColour accentBorderSoft;   ///< H141 accent 30% 预合成（wf-chip 描边）
    wxColour dangerBorder;       ///< H100(.28)/H160(.32) danger 预合成
    wxColour okBorder;           ///< H162 brand 32% 预合成
    wxColour warnBorder;         ///< H128(.35)/H163(.32)/H186(.30) warn 预合成
    wxColour mutedBorder;        ///< H229 rgba(110,109,103,.35) 预合成（虚线思考链）
    wxColour scrollThumb;        ///< H138/H210 rgba(110,109,103,.28) 预合成（细滚动条）
    wxColour glassPaneTop;       ///< H192/H214 白 .80 预合成（对话窗 / 回答气泡顶）
    wxColour glassPaneBottom;    ///< H192 白 .60 预合成（对话窗底）
    wxColour glassRespBottom;    ///< H214 白 .58 预合成（回答气泡底）
    wxColour glassRailTop;       ///< H311 白 .82 预合成（rail 渐变顶）
    wxColour glassRailBottom;    ///< H311 白 .62 预合成（rail 渐变底）
    wxColour glassChain;         ///< H229 白 .62 预合成（思考链卡）
    wxColour glassCard;          ///< H242 白 .66 预合成（计划卡）
    wxColour glassInput;         ///< H255 白 .78 预合成（输入壳）
    wxColour glassChip;          ///< H227 白 .72 预合成（追问 chip）
    wxColour glassEdgeHighlight; ///< H214/H242 白 .85 预合成（1px 高光边）
    wxColour toastBg;            ///< H327 rgba(29,29,31,.92) 预合成（toast 胶囊）
    wxColour textOnToast;        ///< H327 #FFFFFF toast 文字
    wxColour bubbleShadow;       ///< H212 rgba(0,83,180,.30) 用户气泡投影
    wxColour respShadow;         ///< H214 rgba(29,29,31,.05) 回答气泡投影
    wxColour petShadow;          ///< H283 rgba(29,29,31,.14) 桌宠投影（**投影**，随主题推导；不属于 artwork 固定色）
    wxColour petBlueTop;         ///< H467 #7FB6FF 蓝球高光（插画色，不随主题）
    wxColour petBlueMid;         ///< H468 #2E86F5
    wxColour petBlueBottom;      ///< H469 #0561C9
    wxColour petRedTop;          ///< H472 #E46A6A
    wxColour petRedMid;          ///< H473 #C73838
    wxColour petRedBottom;       ///< H474 #9E2A2A
    wxColour petHighlight;       ///< H482/H489 白高光 #FFFFFF
    wxColour petGround;          ///< H464 #1D1D1F 7% 地面椭圆
    wxColour petTie;             ///< H478 #26813D 绿色连线（**artwork 固定值**，不随主题）

    // ---- AGUI-T04 修正轮：语义边线的**逐档** token ----
    // 原型里语义边线的 alpha 是散值，**不得合并**（5% 通道差最多可达 13 个 8bit 台阶，
    // 会改变边框可见度）。命名按「族 + 强度」，注释给出该族的全部取值与出现行：
    //   accent  : .30 H141 | .35 H161 | .40 H126/H189/H204/H252/H318 | .45 H236
    //   danger  : .28 H100 | .32 H160
    //   ok/green: .32 H162
    //   warn    : .30 H186 | .32 H163 | .35 H128
    // 深色档一律 CompositeOver(同族色, 同 alpha, pageBg) 推导，未做视觉对照（DEC-11）。
    wxColour borderAccentMid;    ///< H161 rgba(0,122,255,.35) `.pill.blue`
    wxColour borderAccentStrong; ///< H236 rgba(0,122,255,.45) `.cstep .tick`（1.4px 环）
    wxColour borderDangerWeak;   ///< H100 rgba(199,56,56,.28) `.demo-badge`
    wxColour borderWarnWeak;     ///< H186 rgba(163,90,0,.30) `.chk.warn`
    wxColour borderWarnMid;      ///< H128 rgba(163,90,0,.35) `.step.recheck`
};

/// 指定档的 Agent 扩展色；深色档是推导值，未做视觉对照（DEC-11 待验）
const AgentColours& AgentColoursFor(Mode mode);

/// 当前生效档的 Agent 扩展色
const AgentColours& AgentPalette();

/// 预合成：把 fg 按 alpha 合成到 bg 上。wx 控件底色不参与父层 alpha（THE-021），
/// 因此"玻璃"只能是预合成实色；本函数是 Agent 玻璃/阴影的唯一混合口径。
wxColour CompositeOver(const wxColour& fg, double alpha, const wxColour& bg);

/// Agent 局部几何（单位 DIP；AGUI-T02 最小集，T04 再按组件清单补全）
namespace AgentMetrics {
inline constexpr int CardRadius   = 10;  ///< 卡片 / 消息圆角
inline constexpr int ControlRadius = 6;  ///< 控件圆角
inline constexpr int PillRadius   = 999; ///< 胶囊（实际取 min(w,h)/2）
inline constexpr int Hairline     = 1;   ///< 1 DIP 边线
inline constexpr int FocusRing    = 2;   ///< 原型 2px 焦点描边
inline constexpr int CardMargin   = 2;   ///< 卡片与宿主边缘留白（防圆角包直角）
inline constexpr int CardInset    = 3;   ///< 内容安全内缩（> Hairline，圆角内不露方框）
inline constexpr int ComposerHeight = 40;///< 输入行高度
inline constexpr int ButtonHeight = 38;  ///< 按钮高度
inline constexpr int NarrowButtonHeight = 34; ///< 窄屏按钮
inline constexpr int HeaderHeight = 44;  ///< 对话头
inline constexpr int ThreadMaxWidth = 760; ///< 正文最大宽度
inline constexpr int FloatMargin  = 8;   ///< 浮层 / 桌宠工作区边距
inline constexpr int TopBarHeight = 52;  ///< 试片局部顶栏
inline constexpr int WideBreakpoint   = 1180; ///< DEC-08 宽断点
inline constexpr int MediumBreakpoint = 960;  ///< DEC-08 中断点
inline constexpr int NarrowBreakpoint = 560;  ///< DEC-08 窄断点

// ---- AGUI-T04 追加：逐组件结构尺寸（设计源 index.html；行号见 token-map.md）----
// 只追加，不改上方既有常量（DEC-13）。既有 TopBarHeight=52 是 T02 试片的局部顶栏
// 高度，与设计稿 H89 的 46 不是同一语义，故另设 TitleBarHeight。
inline constexpr int TitleBarHeight      = 46;  ///< H89 .titlebar min-height
inline constexpr int RailWidth           = 58;  ///< H311 .rail width
inline constexpr int RailLogoSize        = 36;  ///< H313 .rail-logo
inline constexpr int RailLogoSizeNarrow  = 26;  ///< H341 ≤960 横条 logo
inline constexpr int RailButton          = 38;  ///< H315 .rb 38×38
inline constexpr int RailButtonNarrow    = 34;  ///< H357 ≤560 .rb
inline constexpr int RailSeparatorWidth  = 24;  ///< H321 .rail-sep
inline constexpr int TopBarDividerHeight = 20;  ///< H323 .tb-sep
inline constexpr int ToolButtonWidth     = 30;  ///< H201 .wt 30×28
inline constexpr int ToolButtonHeight    = 28;  ///< H201
inline constexpr int SendButtonSize      = 40;  ///< H258 .send 40×40
inline constexpr int ColumnLeftWidth     = 216; ///< H105 左列（学习路径）
inline constexpr int ColumnRightWidth    = 336; ///< H105 右列（教学辅助）
inline constexpr int ColumnLeftCompact   = 200; ///< H333 ≤1180
inline constexpr int ColumnRightCompact  = 312; ///< H333 ≤1180
inline constexpr int FloatMaxWidth       = 400; ///< H270 min(400px, calc(100% - 32px))
inline constexpr int FloatMaxHeight      = 596; ///< H270 min(596px, …)
inline constexpr int FloatMaxHeightCompact = 560; ///< H347 ≤960
inline constexpr int FloatInset          = 16;  ///< H270 浮层 top/right 偏移
inline constexpr int FloatInsetCompact   = 8;   ///< H347 ≤960
inline constexpr int UserBubbleMaxPercent = 80; ///< H212 .msg.user max-width:80%
inline constexpr int PetWidth            = 248; ///< H278 .pet width
inline constexpr int PetWidthNarrow      = 200; ///< H354 ≤560
inline constexpr int PetFigureSize       = 170; ///< H282/H283
inline constexpr int PetFigureSizeNarrow = 140; ///< H355
inline constexpr int PetBadgeSize        = 26;  ///< H284 .badge
inline constexpr int PetGoButton         = 30;  ///< H291 .go 30×30
inline constexpr int PetMiniInputHeight  = 30;  ///< H288 .pet-mini input
inline constexpr int PetCapMaxWidth      = 230; ///< H293 .pet-cap max-width
inline constexpr int PetSayTailSize      = 10;  ///< H281 气泡尾巴 10×10
inline constexpr int BubbleTailRadius    = 3;   ///< H212/H214/H280 三角尾圆角
inline constexpr int ScrollBarWidth      = 9;   ///< H137/H209
inline constexpr int ScrollThumbRadius   = 99;  ///< H138/H210
inline constexpr int FocusRingOffset     = 2;   ///< H85 outline-offset:2px
inline constexpr int FocusGlow           = 3;   ///< H257/H287 0 0 0 3px accent-soft
inline constexpr int KvKeyWidth          = 52;  ///< H155/H247 .k width
inline constexpr int SummaryMaxWidth     = 150; ///< H151 .val max-width
inline constexpr int MainMinHeight       = 520; ///< H345 ≤960 mainCol
inline constexpr int SideMinHeight       = 420; ///< H345 ≤960 sideCol
// ---- AGUI-T11 追加：响应式布局派生尺寸（原型未给常量，由三栏网格/桌宠容器推导）----
inline constexpr int MainMinWidth        = 320; ///< 三栏降级单列的最小主区宽度（防横溢）
inline constexpr int PetHeight           = 240; ///< 桌宠容器高（T08 占位气泡沿用；T20 细化）
inline constexpr int PetHeightNarrow     = 200; ///< ≤560 桌宠容器高（与 PetWidthNarrow 同步）
}  // namespace AgentMetrics

// ---------------------------------------------------------------------------
// Agent 局部间距（AGUI-T04）
//
// 单位：CSS px == DIP（设计稿是 1:1 逻辑像素，未按 DPR 折算；spec §13-4）。
// 原生侧一律再经 wxWindow::FromDIP 换算（见 main/agent/gui/AgentTokens.*）。
// 后缀 X = 左右内距，Y = 上下内距。
// ---------------------------------------------------------------------------
namespace AgentSpacing
{
inline constexpr int PagePadding        = 14;  ///< H105 .workarea padding
inline constexpr int ColumnGap          = 14;  ///< H105 grid gap
inline constexpr int TopBarGap          = 14;  ///< H89 .titlebar gap
inline constexpr int TopBarPadX         = 16;  ///< H89 .titlebar padding
inline constexpr int TopBarRightGap     = 10;  ///< H99 .bar-right gap
inline constexpr int BrandGap           = 8;   ///< H93 .brand gap
inline constexpr int LightsGap          = 8;   ///< H90 .lights gap
inline constexpr int SegPad             = 3;   ///< H96 .mode-seg padding
inline constexpr int SegGap             = 2;   ///< H96 .mode-seg gap
inline constexpr int SegBtnPadX         = 14;  ///< H97 .mode-seg button
inline constexpr int SegBtnPadY         = 4;   ///< H97
inline constexpr int LevelGap           = 3;   ///< H324 .lv-seg gap
inline constexpr int LevelPadX          = 9;   ///< H250 .level padding
inline constexpr int LevelPadY          = 4;   ///< H250
inline constexpr int BadgePadX          = 10;  ///< H100 .demo-badge padding
inline constexpr int BadgePadY          = 4;   ///< H100
inline constexpr int StatusPadX         = 11;  ///< H101 .status padding
inline constexpr int StatusPadY         = 4;   ///< H101
inline constexpr int RailPadY           = 10;  ///< H311 .rail padding
inline constexpr int RailPadX           = 6;   ///< H311
inline constexpr int RailGap            = 6;   ///< H311 .rail gap
inline constexpr int RailSepMargin      = 3;   ///< H321 .rail-sep margin
inline constexpr int PaneHeadPadX       = 13;  ///< H116/H139 .pane-h / .sup-h
inline constexpr int PaneHeadPadY       = 11;  ///< H116/H139
inline constexpr int HeadGap            = 8;   ///< H116/H193 头内 gap
inline constexpr int StepsPad           = 8;   ///< H119 .steps padding
inline constexpr int StepGap            = 3;   ///< H119 .steps gap
inline constexpr int StepPadX           = 9;   ///< H120 .step padding
inline constexpr int StepPadY           = 8;   ///< H120
inline constexpr int StepInnerGap       = 9;   ///< H120 .step gap
inline constexpr int AccHeadPadX        = 13;  ///< H145 .acc summary padding
inline constexpr int AccHeadPadY        = 10;  ///< H145
inline constexpr int AccBodyPadX        = 13;  ///< H153 .acc-b padding
inline constexpr int AccBodyPadBottom   = 12;  ///< H153
inline constexpr int AccBodyGap         = 8;   ///< H153 .acc-b gap
inline constexpr int HintPadTop         = 7;   ///< H158 .hintline padding-top
inline constexpr int RowWrapGap         = 6;   ///< H157 .rowwrap gap
inline constexpr int ChipPadX           = 12;  ///< H227 .chip padding
inline constexpr int ChipPadY           = 5;   ///< H227
inline constexpr int ActPadX            = 10;  ///< H179 .act padding
inline constexpr int ActPadY            = 5;   ///< H179
inline constexpr int PillPadX           = 9;   ///< H159 .pill padding
inline constexpr int PillPadY           = 3;   ///< H159
inline constexpr int PillGap            = 5;   ///< H159 .pill gap
inline constexpr int TagPadX            = 8;   ///< H118/H199 .tag padding:2px 8px
inline constexpr int TagPadY            = 2;   ///< H118/H199
inline constexpr int TagGap             = 5;   ///< H100/H199 圆点与文字间距
inline constexpr int CitePadX           = 9;   ///< H174 .cite li padding
inline constexpr int CitePadY           = 7;   ///< H174
inline constexpr int ThreadPadX         = 16;  ///< H208 max(16px,…) 内容列左侧基准
inline constexpr int ThreadPadY         = 14;  ///< H208 .aw-body padding
inline constexpr int ThreadGap          = 13;  ///< H208 .aw-body gap
inline constexpr int AgentMsgGap        = 10;  ///< H213 .msg.agent gap
inline constexpr int BubblePadX         = 13;  ///< H212 .msg.user padding
inline constexpr int BubblePadY         = 9;   ///< H212
inline constexpr int RespPadX           = 14;  ///< H214 .resp padding
inline constexpr int RespPadY           = 11;  ///< H214
inline constexpr int RespListGap        = 6;   ///< H217 .resp ul gap
inline constexpr int RespListIndent     = 15;  ///< H218 padding-left
inline constexpr int TableCellPadX      = 7;   ///< H221 .resp th/td padding
inline constexpr int TableCellPadY      = 6;   ///< H221
inline constexpr int EvidenceGapX       = 12;  ///< H223 .evid gap 6px 12px
inline constexpr int EvidenceGapY       = 6;   ///< H223
inline constexpr int ChainHeadPadX      = 12;  ///< H230 .chain-h padding
inline constexpr int ChainHeadPadY      = 9;   ///< H230
inline constexpr int ChainBodyPadX      = 12;  ///< H233 .chain-steps padding
inline constexpr int ChainBodyPadBottom = 11;  ///< H233
inline constexpr int CardHeadPadX       = 12;  ///< H243 .card-h padding
inline constexpr int CardHeadPadY       = 8;   ///< H243
inline constexpr int CardBodyPadX       = 12;  ///< H245 .card-b padding
inline constexpr int CardBodyPadY       = 10;  ///< H245
inline constexpr int CardBodyGap        = 8;   ///< H245
inline constexpr int CardActionsPadX    = 12;  ///< H249 .card-actions2 padding
inline constexpr int CardActionsPadY    = 9;   ///< H249
inline constexpr int ComposePadX        = 14;  ///< H254 .aw-compose padding
inline constexpr int ComposePadY        = 11;  ///< H254
inline constexpr int ComposePadBottom   = 13;  ///< H254
inline constexpr int ComposeGap         = 8;   ///< H254
inline constexpr int FieldPadX          = 13;  ///< H255 input padding
inline constexpr int StatusStripPadX    = 14;  ///< H205 .aw-status padding
inline constexpr int StatusStripPadY    = 6;   ///< H205
inline constexpr int ToolsGap           = 4;   ///< H200 .aw-tools gap
inline constexpr int PetSayPadX         = 12;  ///< H280 .pet-say padding
inline constexpr int PetSayPadY         = 9;   ///< H280
inline constexpr int PetSayMarginBottom = 12;  ///< H280
inline constexpr int PetSayTailLeft     = 26;  ///< H281 ::after left
inline constexpr int PetMiniGap         = 7;   ///< H286 .pet-mini gap
inline constexpr int PetMiniMarginTop   = 14;  ///< H286
inline constexpr int PetMiniPadLeft     = 12;  ///< H286 padding-left
inline constexpr int PetCapMarginTop    = 11;  ///< H293
inline constexpr int PetLeft            = 24;  ///< H278 .pet left
inline constexpr int PetBottom          = 16;  ///< H278 .pet bottom
inline constexpr int PetCompactInset    = 8;   ///< H348 ≤960 .pet left/bottom
inline constexpr int ToastTop           = 56;  ///< H327 .toast top
inline constexpr int ToastPadX          = 18;  ///< H327
inline constexpr int ToastPadY          = 9;   ///< H327
inline constexpr int ScrollInset        = 2;   ///< H138 滚动条拇指内缩
}  // namespace AgentSpacing

// ---------------------------------------------------------------------------
// Agent 局部字形（AGUI-T04）
//
// 原型字号是 CSS px，**不是**字体 point（1 CSS px = 1/96 in 的逻辑像素；13px UI 文本
// 约等于 9.75pt）。wx 侧统一按 DIP 像素高度渲染（UiTheme::UiFont 同口径），
// point 换算只作记录用（AgentTokens::CssPxToPoint）。
// 分数字号（10.5/11.5/12.5/13.5）必须保留为 double：取整发生在像素高度换算处。
// ---------------------------------------------------------------------------
namespace AgentTypography
{
inline constexpr double MicroPx     = 10.0;  ///< H131/H171/H177/H244
inline constexpr double TinyPx      = 10.5;  ///< H118/H199/H215/H223/H241/H293
inline constexpr double SmallPx     = 11.0;  ///< H151/H179/H230/H250/H325
inline constexpr double SmallBodyPx = 11.5;  ///< H101/H153/H168/H220/H227/H235/H243/H245
inline constexpr double LabelPx     = 12.0;  ///< H97/H130/H280/H288/H327
inline constexpr double TitlePx     = 12.5;  ///< H117/H140
inline constexpr double BodyPx      = 13.0;  ///< H81 body / H211 .msg / H255 input
inline constexpr double BrandPx     = 13.5;  ///< H94 .brand b / H198 .aw-id b
inline constexpr UiTheme::Weight Regular  = Weight::Normal;   ///< 400 正文
inline constexpr UiTheme::Weight Medium   = Weight::Medium;   ///< 500 H97/H130/H145/H159/H179
inline constexpr UiTheme::Weight Semibold = Weight::Semibold; ///< 600 H94/H100/H117/H198/H215/H243
inline constexpr double LineHeightBody   = 1.72; ///< H211 .msg
inline constexpr double LineHeightText   = 1.60; ///< H133/H153/H235/H280/H293
inline constexpr double LineHeightHint   = 1.55; ///< H158 .hintline
}  // namespace AgentTypography

// ---------------------------------------------------------------------------
// Agent 图标档（AGUI-T04；图形本体属 AGUI-T06 的矢量资源，这里只定尺寸）
// 说明：design-review §3 写的"图标 18/14"中，18 可在 H316 证实；14 在任何图标
// 选择器中**不存在**（最小档是 caret 12 / 证据 11），故以实现源码为准，不引入 14。
// ---------------------------------------------------------------------------
namespace AgentIcon
{
inline constexpr int RailButton    = 18; ///< H316 .rb svg
inline constexpr int HeadTool      = 15; ///< H202 .wt svg
inline constexpr int Composer      = 16; ///< H259 .send svg
inline constexpr int Caret         = 12; ///< H148/H231 caret chevron
inline constexpr int Evidence      = 11; ///< H225 .evid svg
inline constexpr int PetBadge      = 13; ///< H285/H292
inline constexpr int StepTick      = 8;  ///< H123 .step .dot svg
inline constexpr int ChainTick     = 9;  ///< H237 .cstep .tick svg
inline constexpr int DotTiny       = 5;  ///< H185 .chk .d
inline constexpr int DotSmall      = 6;  ///< H164/H206
inline constexpr int DotMedium     = 7;  ///< H102/H166
inline constexpr int WindowLight   = 12; ///< H91 .lights i
inline constexpr int WindowDot     = 10; ///< H195 .dot3 i
inline constexpr int StepDot       = 15; ///< H122 .step .dot
inline constexpr int StepDotRing   = 2;  ///< H122 圆点描边
inline constexpr int ChainDot      = 15; ///< H236 .cstep .tick
inline constexpr int PlanBadge     = 17; ///< H171 .pstep2 .n
}  // namespace AgentIcon

// ---------------------------------------------------------------------------
// Agent 透明度权重（AGUI-T04）
//
// 原型一律原值（rgba / backdrop-filter）；wx 侧**不叠加 alpha**（THE-021），
// 而是用这些权重在 UiTheme::CompositeOver 里预合成实色（见 AgentColours::glass*）。
// 保留本表仅为可追溯与后续自绘 alpha 路径。
// ---------------------------------------------------------------------------
namespace AgentOpacity
{
inline constexpr double Glass          = 0.70; ///< H71 --glass
inline constexpr double GlassStrong    = 0.84; ///< H72 --glass-2
inline constexpr double PaneTop        = 0.80; ///< H192/H214
inline constexpr double PaneBottom     = 0.60; ///< H192
inline constexpr double RespBottom     = 0.58; ///< H214
inline constexpr double RailTop        = 0.82; ///< H311
inline constexpr double RailBottom     = 0.62; ///< H311
inline constexpr double Chain          = 0.62; ///< H229
inline constexpr double Card           = 0.66; ///< H242
inline constexpr double Input          = 0.78; ///< H255
inline constexpr double Chip           = 0.72; ///< H227
inline constexpr double StatusStrip    = 0.60; ///< H205
inline constexpr double CardHeadStrip  = 0.65; ///< H243
inline constexpr double EdgeHighlight  = 0.85; ///< H214/H242 高光边
inline constexpr double ScrollThumb    = 0.28; ///< H138/H210
inline constexpr double AccentBorder   = 0.40; ///< H126/H161/H189/H204/H252/H318
inline constexpr double AccentBorderSoft = 0.30; ///< H141
inline constexpr double DangerBorder   = 0.32; ///< H160（H100 为 .28 同族）
inline constexpr double OkBorder       = 0.32; ///< H162
inline constexpr double WarnBorder     = 0.32; ///< H163
inline constexpr double MutedDashed    = 0.35; ///< H229 虚线
inline constexpr double Toast          = 0.92; ///< H327
inline constexpr double BubbleShadow   = 0.30; ///< H212
inline constexpr double RespShadow     = 0.05; ///< H214
inline constexpr double PetShadow      = 0.14; ///< H283
inline constexpr double DisabledAction = 0.65; ///< H181 .act[disabled]
inline constexpr double DisabledLevel  = 0.55; ///< H253 .level[disabled]
inline constexpr double DisabledSend   = 0.50; ///< H261 .send[disabled]

// 修正轮：语义边线 alpha **逐档**保留（原型散值，不合并；5% 通道差最多 13 个 8bit 台阶）
inline constexpr double AccentBorderMid    = 0.35; ///< H161 .pill.blue
inline constexpr double AccentBorderStrong = 0.45; ///< H236 .cstep .tick
inline constexpr double DangerBorderWeak   = 0.28; ///< H100 .demo-badge
inline constexpr double WarnBorderWeak     = 0.30; ///< H186 .chk.warn
inline constexpr double WarnBorderMid      = 0.35; ///< H128 .step.recheck
}  // namespace AgentOpacity

// ---------------------------------------------------------------------------
// Agent 渐变（AGUI-T04）
//
// 停靠色在 AgentColours（pageBg/pageTint/pageTint2、ctaTop/ctaBottom、
// userBubbleTop/userBubbleBottom）；本命名空间只记录角度与 55% 停靠位。
// wxDC::GradientFillLinear 只支持正交方向 ⇒ 168°/135° 需用两段垂直渐变近似。
// ---------------------------------------------------------------------------
namespace AgentGradient
{
inline constexpr double AccentAngleDeg      = 135.0; ///< H74 --grad-acc
inline constexpr double PageAngleDeg        = 168.0; ///< H75 --grad-page
inline constexpr double UserBubbleAngleDeg  = 135.0; ///< H212
inline constexpr double PageMidStop         = 0.55;  ///< H75 55% 停靠
inline constexpr double CtaHoverBrightness  = 1.06;  ///< H260 .send:hover
inline constexpr double CtaHoverBrightnessStrong = 1.08; ///< H320 .rb.primary:hover
}  // namespace AgentGradient

// ---------------------------------------------------------------------------
// Agent 阴影（AGUI-T04）
//
// ⚠️ wx 侧**没有 blur radius**（SAFE-008）。这里的偏移/模糊/alpha 只是设计权重注记：
// 原生实现只能用实色描边或多层渐变近似，不可声称与 CSS 阴影一致（见 token-map 末节）。
// ---------------------------------------------------------------------------
namespace AgentShadow
{
inline constexpr int FloatOffsetY     = 18; ///< H69 --shadow-float
inline constexpr int FloatBlur        = 46; ///< H69
inline constexpr int FloatOffsetYSoft = 2;  ///< H69 第二层
inline constexpr int FloatBlurSoft    = 8;  ///< H69 第二层
inline constexpr int PopOffsetY       = 10; ///< H70 --shadow-pop
inline constexpr int PopBlur          = 26; ///< H70
inline constexpr int BubbleOffsetY    = 5;  ///< H212 .msg.user
inline constexpr int BubbleBlur       = 16; ///< H212
inline constexpr int RespOffsetY      = 3;  ///< H214 .resp
inline constexpr int RespBlur         = 12; ///< H214
inline constexpr int PetOffsetY       = 12; ///< H283 drop-shadow
inline constexpr int PetBlur          = 20; ///< H283
inline constexpr double FloatAlpha     = 0.16; ///< H69
inline constexpr double FloatAlphaSoft = 0.07; ///< H69
inline constexpr double PopAlpha       = 0.12; ///< H70
inline constexpr double BubbleAlpha    = 0.30; ///< H212
inline constexpr double RespAlpha      = 0.05; ///< H214
inline constexpr double PetAlpha       = 0.14; ///< H283
}  // namespace AgentShadow

// ---------------------------------------------------------------------------
// Agent 动效参数（AGUI-T04：只做 Token 化清单，本层不实现动画）
//
// 动画本体归 AGUI-T20/T27（桌宠、减少动画）。真实事件驱动的界面不得为了等动画
// 而延迟业务结果（design-review §3 末段）。reduced-motion 下原型把 duration 归零
// （H359-360），原生侧等价于"不启动"而不是"0ms 播放"。
// ---------------------------------------------------------------------------
namespace AgentMotion
{
inline constexpr int HoverTool        = 150; ///< H201 .wt .15s
inline constexpr int HoverFast        = 160; ///< H145/H315 .16s
inline constexpr int HoverBase        = 180; ///< H97/H120/H227/H250 .18s
inline constexpr int CaretRotate      = 180; ///< H148 .acc caret
inline constexpr int CaretRotateChain = 200; ///< H231 .chain-h svg
inline constexpr int ToastFade        = 300; ///< H327 .toast transition
inline constexpr int ToastHold        = 2200;///< H677 setTimeout 2200
inline constexpr int PopIn            = 320; ///< H270 animation:pop .32s
inline constexpr int FadeUp           = 300; ///< H211 .msg fadeUp .3s
inline constexpr int FadeUpSoft       = 280; ///< H235/H280 fadeUp .28s
inline constexpr int TypingCycle      = 1050;///< H263 .typing 1.05s
inline constexpr int TypingStagger    = 140; ///< H264 animation-delay .14s
inline constexpr int BounceCycle      = 2400;///< H295 bounce 2.4s
inline constexpr int BounceRedDelay   = 420; ///< H296 .42s
inline constexpr int StretchCycle     = 2400;///< H297 stretch 2.4s
inline constexpr int BlinkCycle       = 4600;///< H301 blink 4.6s
inline constexpr int ThinkCycle       = 1100;///< H300 think 1.1s
inline constexpr int DanceCycle       = 520; ///< H299 dance .52s
inline constexpr int DanceRepeat      = 2;   ///< H299 ×2
inline constexpr int IdlePeriod       = 9000;///< H1131 setInterval 9000
inline constexpr int IdleRestore      = 3400;///< H1129 setTimeout 3400
inline constexpr int ChainStepTick    = 400; ///< H943 演示节拍（非业务延迟）
inline constexpr int ResultDelayPlain = 440; ///< H946
inline constexpr int ResultDelayChain = 560; ///< H946
inline constexpr int ChainCollapseDelay = 240; ///< H936
inline constexpr int FocusDelay       = 60;  ///< H750/H1010/H1071
inline constexpr double PopBezierX1   = 0.3; ///< H270 cubic-bezier(.3,1.2,.4,1)
inline constexpr double PopBezierY1   = 1.2; ///< H270
inline constexpr double PopBezierX2   = 0.4; ///< H270
inline constexpr double PopBezierY2   = 1.0; ///< H270
}  // namespace AgentMotion

// ---------------------------------------------------------------------------
// 订阅句柄（THE-028：必须可解绑；析构自动解绑）
// ---------------------------------------------------------------------------
namespace detail {
unsigned long SubscribeImpl(std::function<void()> callback);
void UnsubscribeImpl(unsigned long id);
}  // namespace detail

class Subscription
{
public:
    Subscription() = default;
    explicit Subscription(unsigned long id) : m_id(id) {}

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept : m_id(other.m_id) { other.m_id = 0; }

    Subscription& operator=(Subscription&& other) noexcept
    {
        if (this != &other) {
            Reset();
            m_id       = other.m_id;
            other.m_id = 0;
        }
        return *this;
    }

    ~Subscription() { Reset(); }

    void Reset()
    {
        if (m_id != 0) {
            detail::UnsubscribeImpl(m_id);
            m_id = 0;
        }
    }

    bool IsBound() const { return m_id != 0; }

private:
    unsigned long m_id = 0;
};

/// 订阅主题变更；返回的句柄析构即解绑（窗口销毁时自动失效）。
inline Subscription Subscribe(std::function<void()> callback)
{
    return Subscription(detail::SubscribeImpl(std::move(callback)));
}

// ---------------------------------------------------------------------------
// 主题状态 API
// ---------------------------------------------------------------------------

/// 当前生效档（System 已解析为 Light/Dark）
Mode EffectiveMode();

/// 用户选择的档（可能是 System）
Mode GetMode();

/// 设置并持久化到 wxConfig 键 `Ui/Theme/Mode`（THE-008）；会通知订阅者。
/// 显式设置为 Light/Dark 后，系统变化不再覆盖用户选择（THE-006）。
void SetMode(Mode mode);

/// 重新探测系统主题并通知订阅者（供 `wxEVT_SYS_COLOUR_CHANGED` 使用，T-120）。
/// 仅在 GetMode() == System 时改变 EffectiveMode()。
void RefreshFromSystem();

/// 字符串 <-> 枚举（wxConfig 取值 `system` / `light` / `dark`）
wxString ModeToString(Mode mode);
Mode ModeFromString(const wxString& value, Mode fallback = Mode::System);

/// 当前生效档的调色板
const Colours& Palette();
wxColour CanvasSeedColour(const wxColour& seed, bool selected = false);

/// 页面底：160° 线性渐变 bgWindow → bgPanel(55%) → bgDeep（app.html §0）。
/// 由「页面」上的各绘制方（AUI dock 背景、区域宿主）共同调用；以**顶层窗口客户区**为锚，
/// 保证多块拼接后仍是一条连续的渐变。
///   clientSize —— 顶层窗口客户区尺寸
///   dcOrigin   —— 当前 DC 原点在**顶层客户区**坐标系里的偏移（帧自绘时传 (0,0)）
///   rect       —— 本次要填充的区域（当前 DC 坐标系）
/// 说明：wxDC::GradientFillLinear 只支持正交方向，因此用**两段垂直**渐变近似 160°；
///       该角度相对垂直只偏 20°，在这种极淡的冷灰上肉眼不可辨。颜色全部取自 token。
void PaintPageGradient(wxDC& dc, const wxSize& clientSize, const wxPoint& dcOrigin,
                       const wxRect& rect);

/// 画布底：白底 `--canvas` + 20 DIP 细网格 `--grid` + 100 DIP 粗网格 `--gridStrong`。
/// 用于"未打开任何文件"的空画布 —— 不能露出笔记本的空灰底。
void PaintCanvasBackdrop(wxDC& dc, const wxRect& rect, const wxWindow* window);

/// 指定档的调色板；传入 System 时按系统探测解析
const Colours& PaletteFor(Mode mode);

/// 固定深底终端的文字/语义色；不改变应用主题，也不解除 DEC-11 门禁。
/// 仅供 termBg 背景的终端使用，避免主题门禁把深底文字降为浅色档。
const Colours& TerminalPalette();

/// 当前生效档的扩展族（教学 / Agent 面板内部专用）
const TeachingColours& Teaching();

/// 系统是否处于深色（探测失败返回 false → 回退 Light）
bool IsSystemDark();

/// 深色档是否已启用（**2026-10-12 起为 true**：DEC-11 的四个恢复条件已满足，见 UiTheme.cpp 说明）
bool IsDarkModeEnabled();

/// 把整棵窗口树里"仍停留在**另一档** token 值"的前景/背景色改写为**当前档**的对应 token。
///
/// 存在理由（T-140 夹具 F-140-01/02/03 实测）：大量**原生控件**的前景色是构造期一次性
/// `SetForegroundColour(Palette().textN)` 写进去的，主题切换后**不会**更新 → 深色档下出现
/// "深壳浅字"（实测主壳 81/134 个文字仍为浅档色，画在 `#0A0B0E` 上几乎不可读）。
///
/// 该函数与切换方向**无关**：它把"等于另一档 token 值"的颜色改成当前档的对应值，
/// 因此可安全重复调用（幂等），也无需记录"上一档"。返回被改写的窗口数（供夹具断言/日志）。
unsigned RemapStaleTokenColours(wxWindow* root);

// ---------------------------------------------------------------------------
// 字体 API（THE-016 / THE-017 / PLT-002）
// ---------------------------------------------------------------------------

/// UI 字体：系统 GUI 字体族 + DIP 像素高度（中文由系统回退保证）
wxFont UiFont(const wxWindow* window, TextSize size, Weight weight = Weight::Normal);

/// 等宽字体：wxFONTFAMILY_TELETYPE（Windows → Consolas / Cascadia Mono）
wxFont MonoFont(const wxWindow* window, TextSize size, Weight weight = Weight::Normal);

// ---------------------------------------------------------------------------
// 自检 / 测试辅助
// ---------------------------------------------------------------------------

/// 覆盖系统探测结果（仅供单元测试）：-1 = 恢复真实探测
void SetSystemDarkOverrideForTest(int state);

/// 返回被引用但为空的 token 数量（自检用：应为 0）
int CountUnsetTokens();

}  // namespace UiTheme
