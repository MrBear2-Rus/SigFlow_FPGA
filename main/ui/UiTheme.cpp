//
// UiTheme.cpp — SigFlow 唯一设计 Token 层实现（T-110）
//
// 取值来源：docs/GUI-CHUGAO/SigFlow-界面改造SPEC.md v2.0 §3.3 / §3.5 / §14.1(DEC-02)
// 溯源键：GUI/app.html:51-96（设计稿 :root 的 --l-* / --d-* 调色板，2026-10-10 冻结）
//
// 注意：本文件的颜色字面量是**唯一合法来源**（SAFE-012）。其他文件一律经 UiTheme 取用。
//

#include "ui/UiTheme.h"

#include <wx/config.h>
#include <wx/settings.h>
#include <wx/brush.h>
#include <wx/dc.h>
#include <wx/graphics.h>

#include <cmath>
#include <memory>
#include <vector>

#ifdef __WXMSW__
#include <wx/fontenum.h>
#include <wx/msw/registry.h>  // wxRegKey（wxUSE_REGKEY=1；注意不是 <wx/regkey.h>）
#endif

namespace UiTheme {
namespace {

// ===========================================================================
// DEC-11（2026-10-10 · 用户实测反馈）：**深色档暂时搁置，锁定浅色**
//
// 原因（用户截图实证）：主窗口 chrome 已 token 化，但 画布 / 树 / 属性表单 / 状态栏 /
//   子窗口（FPGA·TraceBridge·DebugContract，仍走深色 FpgaTheme）尚未 token 化。
//   此时切深色会出现"部分变黑、部分仍白"的割裂，观感差于不切。
//
// 恢复条件（**2026-10-12 已全部满足，故置 true**；条件清单经 T-719 修订）：
//   ① T-310 / T-330 / T-340 / T-410 —— 已落地；
//   ② **T-140**（子窗口接入主题：FpgaTheme 适配层 + 三个独立子窗订阅刷新）—— 本轮落地；
//   ③ THE-030 的三处刷新缺口 —— (a) `center_area` 未在 pane 表 → 已修；(b) 绘制级 TabArt
//      是 `Clone()` 出的基类拷贝、永不取 token → 已修（`UiTabArt::Clone()` + 绘制入口自刷新）；
//      (c) 已注册面板刷新遍历 `reached=0/8` → 已修（`ApplyTheme` 子树遍历 + 各面板订阅）；
//   ④ DEC-01 的 Pin Binding 页硬编码 `#1E1E20` → 已改为 token。
//   UI 入口（Window › Theme）会随本开关自动出现（MainMenuBar 按它判断）。
// ===========================================================================
constexpr bool kDarkModeEnabled = true;

// ===========================================================================
// 基础族：浅色档（SPEC §3.3）
// ===========================================================================
Colours MakeLight()
{
    Colours c;

    // ---- 底色 ----
    c.bgWindow = wxColour(247, 247, 250);  // #f7f7fa（渐变上半略微加深，但不深于下半 #f2f2f6）  // #fbfbfd
    c.bgPanel  = wxColour(250, 250, 252);  // #f5f5f7
    c.bgDeep   = wxColour(242, 242, 246);  // #e8e8ed

    // ---- 面板 / 交互面（保留原始 alpha；仅自绘路径可用，THE-021）----
    c.surface       = wxColour(245, 245, 247, 168);  // rgba(245,245,247,.66)
    c.surfaceStrong = wxColour(255, 255, 255, 219);  // rgba(255,255,255,.86)
    c.surfaceSunken = wxColour(29, 29, 31, 11);      // rgba(29,29,31,.045)

    // ---- 描边与高光 ----
    c.stroke       = wxColour(18, 24, 40, 26);   // rgba(18,24,40,.10)
    c.strokeStrong = wxColour(18, 24, 40, 46);   // rgba(18,24,40,.18)
    c.specular     = wxColour(255, 255, 255, 230);  // rgba(255,255,255,.90)

    // ---- 文字 ----
    c.text1 = wxColour(29, 29, 31);    // #1d1d1f
    c.text2 = wxColour(86, 90, 102);   // #565a66
    c.text3 = wxColour(135, 140, 153); // #878c99

    // ---- 画布 ----
    c.canvas     = wxColour(255, 255, 255);  // #ffffff
    c.grid       = wxColour(238, 240, 244);  // #eef0f4
    c.gridStrong = wxColour(224, 228, 234);  // #e0e4ea
    c.wire       = wxColour(28, 31, 38);     // #1c1f26

    // ---- 语义色 ----
    c.brand  = wxColour(31, 138, 76);  // #1f8a4c
    c.accent = wxColour(0, 113, 227);  // #0071e3
    c.warn   = wxColour(178, 80, 0);   // #b25000
    c.danger = wxColour(193, 18, 31);  // #c1121f

    // ---- 状态底 ----
    c.sel   = wxColour(0, 113, 227, 28);   // rgba(0,113,227,.11)
    c.hover = wxColour(16, 24, 45, 13);    // rgba(16,24,45,.05)

    // ---- 终端（SPEC §3.5 补充表）----
    c.termBg = wxColour(13, 13, 15);  // #0d0d0f

    // T-240：GUI/app.html 的唯一液态玻璃例外（无真实 blur）。
    c.glassEdge = wxColour(255, 255, 255, 204);
    c.glassEdgeStrong = wxColour(255, 255, 255, 230);
    c.glassEdgeSoft = wxColour(255, 255, 255, 140);
    c.glassRim = wxColour(255, 255, 255, 51);
    c.glassRing = wxColour(0, 0, 0, 204);
    c.glassShadow = wxColour(0, 0, 0, 51);
    c.canvasShadow = wxColour(0, 0, 0, 41);
    c.glassBorder = wxColour(51, 51, 51, 20);

    // ---- 合成实色（SPEC §3.5，工程适配建议值）----
    c.surfaceOnWindow     = wxColour(247, 247, 249);  // #f7f7f9
    c.surfaceOnPanel      = wxColour(250, 250, 252);  // #f5f5f7
    c.surfaceOnCanvas     = wxColour(247, 247, 248);  // #f7f7f8
    c.strongOnPanel       = wxColour(254, 254, 254);  // #fefefe
    c.sunkenOnPanel       = wxColour(235, 235, 237);  // #ebebed
    c.sunkenOnCanvas      = wxColour(245, 245, 245);  // #f5f5f5
    c.strokeOnWindow      = wxColour(233, 233, 236);  // #e9e9ec
    c.strokeOnPanel       = wxColour(222, 223, 226);  // #dedfe2
    c.strokeStrongOnWindow = wxColour(215, 215, 219); // #d7d7db
    c.strokeStrongOnPanel  = wxColour(207, 208, 212); // #cfd0d4
    c.selOnPanel          = wxColour(218, 230, 243);  // #dae6f3
    c.hoverOnPanel        = wxColour(234, 234, 237);  // #eaeaed
    c.focusRingOnPanel    = wxColour(135, 186, 238);  // #87baee

    // 代码专用派生 token（SPEC §3.6）：在白底上达到 4.5:1
    c.codeType    = wxColour(31, 134, 76);    // #1f864c 4.59:1
    c.codeComment = wxColour(113, 118, 127);  // #71767f 4.56:1

    return c;
}

// ===========================================================================
// 基础族：深色档（SPEC §3.3）
// ===========================================================================
Colours MakeDark()
{
    Colours c;

    c.bgWindow = wxColour(10, 11, 14);  // #0a0b0e
    c.bgPanel  = wxColour(17, 18, 22);  // #111216
    c.bgDeep   = wxColour(5, 5, 6);     // #050506

    c.surface       = wxColour(33, 34, 38, 153);  // rgba(33,34,38,.60)
    c.surfaceStrong = wxColour(41, 42, 47, 204);  // rgba(41,42,47,.80)
    c.surfaceSunken = wxColour(0, 0, 0, 77);      // rgba(0,0,0,.30)

    c.stroke       = wxColour(255, 255, 255, 26);  // rgba(255,255,255,.10)
    c.strokeStrong = wxColour(255, 255, 255, 46);  // rgba(255,255,255,.18)
    c.specular     = wxColour(255, 255, 255, 41);  // rgba(255,255,255,.16)

    c.text1 = wxColour(244, 245, 247);  // #f4f5f7
    c.text2 = wxColour(154, 157, 168);  // #9a9da8
    c.text3 = wxColour(111, 115, 128);  // #6f7380

    c.canvas     = wxColour(20, 21, 25);   // #141519
    c.grid       = wxColour(30, 32, 38);   // #1e2026
    c.gridStrong = wxColour(39, 42, 49);   // #272a31
    c.wire       = wxColour(215, 218, 226); // #d7dae2

    c.brand  = wxColour(62, 211, 124);  // #3ed37c
    c.accent = wxColour(10, 132, 255);  // #0a84ff
    c.warn   = wxColour(255, 159, 10);  // #ff9f0a
    c.danger = wxColour(255, 91, 82);   // #ff5b52

    c.sel   = wxColour(10, 132, 255, 46);  // rgba(10,132,255,.18)
    c.hover = wxColour(255, 255, 255, 15); // rgba(255,255,255,.06)

    c.termBg = wxColour(13, 13, 15);  // #0d0d0f（不随档位变化）
    c.glassEdge = wxColour(255, 255, 255, 77);
    c.glassEdgeStrong = wxColour(255, 255, 255, 107);
    c.glassEdgeSoft = wxColour(255, 255, 255, 46);
    c.glassRim = wxColour(255, 255, 255, 36);
    c.glassRing = wxColour(0, 0, 0, 166);
    c.glassShadow = wxColour(0, 0, 0, 140);
    c.canvasShadow = wxColour(0, 0, 0, 41);
    c.glassBorder = wxColour(255, 255, 255, 31);

    // ---- 合成实色（深色档，SPEC §3.5）----
    c.surfaceOnWindow      = wxColour(23, 24, 27);   // #17181b
    c.surfaceOnPanel       = wxColour(24, 25, 28);   // #18191c
    c.surfaceOnCanvas      = wxColour(24, 26, 29);   // #181a1d
    c.strongOnPanel        = wxColour(36, 37, 42);   // #24252a
    c.sunkenOnPanel        = wxColour(12, 12, 14);   // #0c0c0e
    c.sunkenOnCanvas       = wxColour(14, 15, 18);   // #0e0f12
    c.strokeOnWindow       = wxColour(39, 40, 43);   // #27282b
    c.strokeOnPanel        = wxColour(41, 42, 45);   // #292a2d
    c.strokeStrongOnWindow = wxColour(58, 59, 62);   // #3a3b3e
    c.strokeStrongOnPanel  = wxColour(60, 61, 64);   // #3c3d40
    c.selOnPanel           = wxColour(16, 39, 64);   // #102740
    c.hoverOnPanel         = wxColour(31, 32, 36);   // #1f2024
    c.focusRingOnPanel     = wxColour(14, 69, 127);  // #0e457f

    // 代码专用派生 token（SPEC §3.6）：在 canvas #141519 上达到 4.5:1
    c.codeType    = wxColour(62, 211, 124);   // #3ed37c 9.40:1（与 brand 同值）
    c.codeComment = wxColour(124, 130, 144);  // #7c8290 4.74:1

    return c;
}

// ===========================================================================
// 扩展族：教学 / Agent 状态语义色（DEC-02；来源 docs/gui/...:123-127）
// 使用边界：仅 Agent / 教学面板内部（SPEC §3.3 末表）
// ===========================================================================
TeachingColours MakeTeachingLight()
{
    TeachingColours t;
    t.guideBg    = wxColour(230, 241, 251);  // #E6F1FB
    t.guideText  = wxColour(12, 68, 124);    // #0C447C
    t.doneBg     = wxColour(234, 243, 222);  // #EAF3DE
    t.doneText   = wxColour(39, 80, 10);     // #27500A
    t.hardwareBg = wxColour(250, 238, 218);  // #FAEEDA
    t.hardwareText = wxColour(113, 43, 19);  // #712B13
    t.errorBg    = wxColour(252, 235, 235);  // #FCEBEB
    t.errorText  = wxColour(121, 31, 31);    // #791F1F
    t.stateOk    = wxColour(38, 129, 61);    // #26813D
    t.stateWarn  = wxColour(163, 90, 0);     // #A35A00
    t.stateBad   = wxColour(199, 56, 56);    // #C73838
    return t;
}

TeachingColours MakeTeachingDark()
{
    TeachingColours t;
    t.guideBg    = wxColour(33, 54, 77);     // #21364D
    t.guideText  = wxColour(190, 220, 255);  // #BEDCFF
    t.doneBg     = wxColour(39, 60, 43);     // #273C2B
    t.doneText   = wxColour(191, 227, 173);  // #BFE3AD
    t.hardwareBg = wxColour(70, 50, 38);     // #463226
    t.hardwareText = wxColour(255, 208, 164); // #FFD0A4
    t.errorBg    = wxColour(71, 42, 48);     // #472A30
    t.errorText  = wxColour(255, 193, 193);  // #FFC1C1
    t.stateOk    = wxColour(116, 211, 138);  // #74D38A
    t.stateWarn  = wxColour(255, 195, 107);  // #FFC36B
    t.stateBad   = wxColour(255, 141, 141);  // #FF8D8D
    return t;
}

const Colours& LightPalette()
{
    static const Colours p = MakeLight();
    return p;
}

const Colours& DarkPalette()
{
    static const Colours p = MakeDark();
    return p;
}

const TeachingColours& TeachingLight()
{
    static const TeachingColours t = MakeTeachingLight();
    return t;
}

const TeachingColours& TeachingDark()
{
    static const TeachingColours t = MakeTeachingDark();
    return t;
}

// ===========================================================================
// Agent 局部扩展（DEC-13 / AGUI-T02）
//
// 取值来源：GUI/agent-gui/index.html §1 的 CSS 变量；玻璃白 .70/.84 在此**预合成**
// 为实色（SAFE-008：wx 侧不做真实模糊）。深色档不新增色相，全部由既有深色基础族
// 与教学扩展族推导，未做视觉对照（DEC-11 / A01 待验）。
// 混合口径 CompositeOver 是 UiTheme 的公开函数（定义在本文件命名空间外层）。
// ===========================================================================

// ===========================================================================
// 桌宠 artwork 固定色（AGUI-T04 修正轮 / VIS-008）
//
// 桌宠是**插画**，不是主题色：原型里双球渐变（#7FB6FF→#2E86F5→#0561C9、
// #E46A6A→#C73838→#9E2A2A H467-H474）、绿色连线 #26813D（H478）、白高光
// （H482/H489）与地面椭圆 rgba(29,29,31,.07)（H464）都不随亮/暗档变化。
// 因此浅色与深色**共用这一个函数**赋值 —— 从构造上保证两档逐字节同值，
// 不允许任何一档把它绑到 brand/accent 之类的主题 token（曾误绑 brand，
// 导致深色档绿线变成 #74D38A）。
//
// petGround 保留原始 alpha（wxColour(29,29,31,18)）：它是插画像素，不参与
// THE-021 的"控件底色预合成"。绘制方必须用 wxGraphicsContext（GDI 的 wxDC
// 会忽略 alpha，画成实心深块）。
// ===========================================================================
void ApplyPetArtwork(AgentColours& a)
{
    a.petBlueTop    = wxColour(127, 182, 255);   // #7FB6FF H467
    a.petBlueMid    = wxColour(46, 134, 245);    // #2E86F5 H468
    a.petBlueBottom = wxColour(5, 97, 201);      // #0561C9 H469
    a.petRedTop     = wxColour(228, 106, 106);   // #E46A6A H472
    a.petRedMid     = wxColour(199, 56, 56);     // #C73838 H473
    a.petRedBottom  = wxColour(158, 42, 42);     // #9E2A2A H474
    a.petHighlight  = wxColour(255, 255, 255);   // #FFFFFF H482/H489
    a.petGround     = wxColour(29, 29, 31, 18);  // rgba(29,29,31,.07) H464（.07×255≈18）
    a.petTie        = wxColour(38, 129, 61);     // #26813D H478
}

AgentColours MakeAgentLight()
{
    AgentColours a;
    a.pageBg     = wxColour(250, 250, 248);  // #FAFAF8
    a.pageTint   = wxColour(239, 244, 250);  // #EFF4FA
    a.pageTint2  = wxColour(243, 246, 249);  // #F3F6F9
    a.cardBg     = wxColour(255, 255, 255);  // #FFFFFF
    a.hairline   = wxColour(229, 229, 229);  // #E5E5E5
    a.text1      = wxColour(29, 29, 31);     // #1D1D1F
    a.text2      = wxColour(95, 94, 90);     // #5F5E5A
    a.text3      = wxColour(110, 109, 103);  // #6E6D67
    a.accent     = wxColour(0, 122, 255);    // #007AFF
    a.accentText = wxColour(12, 68, 124);    // #0C447C
    a.accentSoft = wxColour(230, 241, 251);  // #E6F1FB
    a.brand      = wxColour(38, 129, 61);    // #26813D
    a.okSoft     = wxColour(234, 243, 222);  // #EAF3DE
    a.warn       = wxColour(163, 90, 0);     // #A35A00
    a.warnSoft   = wxColour(250, 238, 218);  // #FAEEDA
    a.danger     = wxColour(199, 56, 56);    // #C73838
    a.dangerSoft = wxColour(252, 235, 235);  // #FCEBEB
    a.userBubbleTop    = wxColour(10, 114, 220);  // #0A72DC
    a.userBubbleBottom = wxColour(0, 83, 180);    // #0053B4
    a.textOnAccent     = wxColour(255, 255, 255); // 蓝底白字（对比度见 T02 报告）
    a.ctaTop     = wxColour(61, 149, 255);   // #3D95FF
    a.ctaBottom  = wxColour(0, 122, 255);    // #007AFF

    a.cardGlass       = CompositeOver(a.cardBg, 0.70, a.pageBg);
    a.cardGlassStrong = CompositeOver(a.cardBg, 0.84, a.pageBg);
    a.shadow          = CompositeOver(wxColour(29, 29, 31), 0.16, a.pageBg);

    // ---- AGUI-T04 追加（浅色精确值；来源行号见 GUI/agent-gui/phase1/token-map.md）----
    // 透明色一律 CompositeOver 预合成：wx 不做真实模糊，也不叠加父层 alpha（THE-021）。
    a.sunken           = wxColour(241, 239, 232);  // #F1EFE8 H48
    a.sunkenSoft       = CompositeOver(a.sunken, 0.60, a.pageBg);  // H205 rgba(241,239,232,.60)
    a.sunkenSoftStrong = CompositeOver(a.sunken, 0.65, a.pageBg);  // H243 rgba(241,239,232,.65)
    a.dangerText = wxColour(121, 31, 31);   // #791F1F H57
    a.okText     = wxColour(39, 80, 10);    // #27500A H60
    a.warnText   = wxColour(113, 43, 19);   // #712B13 H63

    a.accentBorder     = CompositeOver(a.accent, 0.40, a.pageBg); // H126/H161/H189/H204/H252/H318
    a.accentBorderSoft = CompositeOver(a.accent, 0.30, a.pageBg); // H141
    a.dangerBorder     = CompositeOver(a.danger, 0.32, a.pageBg); // H160（H100 为 .28 同族）
    a.okBorder         = CompositeOver(a.brand, 0.32, a.pageBg);  // H162
    a.warnBorder       = CompositeOver(a.warn, 0.32, a.pageBg);   // H163（H128 .35 / H186 .30）
    a.mutedBorder      = CompositeOver(a.text3, 0.35, a.pageBg);  // H229 rgba(110,109,103,.35)
    a.scrollThumb      = CompositeOver(a.text3, 0.28, a.pageBg);  // H138/H210

    a.glassPaneTop       = CompositeOver(a.cardBg, 0.80, a.pageBg); // H192/H214
    a.glassPaneBottom    = CompositeOver(a.cardBg, 0.60, a.pageBg); // H192
    a.glassRespBottom    = CompositeOver(a.cardBg, 0.58, a.pageBg); // H214
    a.glassRailTop       = CompositeOver(a.cardBg, 0.82, a.pageBg); // H311
    a.glassRailBottom    = CompositeOver(a.cardBg, 0.62, a.pageBg); // H311
    a.glassChain         = CompositeOver(a.cardBg, 0.62, a.pageBg); // H229
    a.glassCard          = CompositeOver(a.cardBg, 0.66, a.pageBg); // H242
    a.glassInput         = CompositeOver(a.cardBg, 0.78, a.pageBg); // H255
    a.glassChip          = CompositeOver(a.cardBg, 0.72, a.pageBg); // H227
    a.glassEdgeHighlight = CompositeOver(a.cardBg, 0.85, a.pageBg); // H214/H242 1px 高光边

    a.toastBg     = CompositeOver(wxColour(29, 29, 31), 0.92, a.pageBg); // H327
    a.textOnToast = wxColour(255, 255, 255);                             // H327 白字
    a.bubbleShadow = CompositeOver(wxColour(0, 83, 180), 0.30, a.pageBg); // H212
    a.respShadow   = CompositeOver(a.text1, 0.05, a.pageBg);             // H214
    a.petShadow    = CompositeOver(a.text1, 0.14, a.pageBg);             // H283

    // 桌宠插画色（H397/H467–H474/H482/H489）：固定 artwork，不随主题变化。
    // 桌宠 artwork：与深色档**共用** ApplyPetArtwork()，从构造上保证同值。
    ApplyPetArtwork(a);

    // ---- 修正轮：语义边线逐档（原型 alpha 不合并）----
    a.borderAccentMid    = CompositeOver(a.accent, 0.35, a.pageBg);  // H161
    a.borderAccentStrong = CompositeOver(a.accent, 0.45, a.pageBg);  // H236
    a.borderDangerWeak   = CompositeOver(a.danger, 0.28, a.pageBg);  // H100
    a.borderWarnWeak     = CompositeOver(a.warn, 0.30, a.pageBg);    // H186
    a.borderWarnMid      = CompositeOver(a.warn, 0.35, a.pageBg);    // H128
    return a;
}

AgentColours MakeAgentDark()
{
    const Colours& base = DarkPalette();
    const TeachingColours& teach = TeachingDark();

    AgentColours a;
    a.pageBg     = base.bgWindow;
    a.pageTint   = base.bgPanel;
    a.pageTint2  = base.bgDeep;
    a.cardBg     = base.bgPanel;
    a.hairline   = base.stroke;
    a.text1      = base.text1;
    a.text2      = base.text2;
    a.text3      = base.text3;
    a.accent     = base.accent;
    a.accentText = base.accent;
    a.accentSoft = CompositeOver(base.accent, 0.18, base.bgPanel);
    a.brand      = teach.stateOk;
    a.okSoft     = CompositeOver(teach.stateOk, 0.20, base.bgPanel);
    a.warn       = teach.stateWarn;
    a.warnSoft   = CompositeOver(teach.stateWarn, 0.20, base.bgPanel);
    a.danger     = teach.stateBad;
    a.dangerSoft = CompositeOver(teach.stateBad, 0.18, base.bgPanel);
    a.userBubbleTop    = CompositeOver(base.accent, 0.80, base.bgPanel);
    a.userBubbleBottom = CompositeOver(base.accent, 0.55, base.bgDeep);
    a.textOnAccent     = wxColour(255, 255, 255);
    a.ctaTop     = CompositeOver(base.accent, 0.80, base.bgPanel);
    a.ctaBottom  = base.accent;

    a.cardGlass       = CompositeOver(a.cardBg, 0.70, a.pageBg);
    a.cardGlassStrong = CompositeOver(a.cardBg, 0.84, a.pageBg);
    a.shadow          = CompositeOver(wxColour(0, 0, 0), 0.45, a.pageBg);

    // ---- AGUI-T04 追加：深色档**不新增色相**，全部由既有深色基础族 / 教学扩展族
    //      经 CompositeOver 推导；未做视觉对照（DEC-11 / A01 待验）。----
    //
    // 下沉面：既有深色 token 里 sunkenOnPanel(#0c0c0e) 与 pageBg(#0a0b0e) 几乎不可分，
    // 无法承担"状态条/卡头"的可见分层，故取既有 surfaceOnPanel 作为下沉面。
    a.sunken           = base.surfaceOnPanel;                      // #18191c
    a.sunkenSoft       = CompositeOver(a.sunken, 0.60, a.pageBg);
    a.sunkenSoftStrong = CompositeOver(a.sunken, 0.65, a.pageBg);
    a.dangerText = teach.errorText;      // #FFC1C1
    a.okText     = teach.doneText;       // #BFE3AD
    a.warnText   = teach.hardwareText;   // #FFD0A4

    a.accentBorder     = CompositeOver(a.accent, 0.40, a.pageBg);
    a.accentBorderSoft = CompositeOver(a.accent, 0.30, a.pageBg);
    a.dangerBorder     = CompositeOver(a.danger, 0.32, a.pageBg);
    a.okBorder         = CompositeOver(a.brand, 0.32, a.pageBg);
    a.warnBorder       = CompositeOver(a.warn, 0.32, a.pageBg);
    a.mutedBorder      = CompositeOver(base.text3, 0.35, a.pageBg);
    a.scrollThumb      = CompositeOver(base.text3, 0.28, a.pageBg);

    // 玻璃：沿用设计 alpha，但"玻璃颜料"取既有深色 surfaceStrong 的 RGB，
    // 否则深色档会得到一层发白的雾面（白色 .80 叠在 #0a0b0e 上）。
    const wxColour glassPigment = base.surfaceStrong;
    a.glassPaneTop       = CompositeOver(glassPigment, 0.80, a.pageBg);
    a.glassPaneBottom    = CompositeOver(glassPigment, 0.60, a.pageBg);
    a.glassRespBottom    = CompositeOver(glassPigment, 0.58, a.pageBg);
    a.glassRailTop       = CompositeOver(glassPigment, 0.82, a.pageBg);
    a.glassRailBottom    = CompositeOver(glassPigment, 0.62, a.pageBg);
    a.glassChain         = CompositeOver(glassPigment, 0.62, a.pageBg);
    a.glassCard          = CompositeOver(glassPigment, 0.66, a.pageBg);
    a.glassInput         = CompositeOver(glassPigment, 0.78, a.pageBg);
    a.glassChip          = CompositeOver(glassPigment, 0.72, a.pageBg);
    // 高光边：沿用既有深色 specular 的 alpha（0.16）作为"白高光"的强度，不另造权重。
    a.glassEdgeHighlight =
        CompositeOver(wxColour(255, 255, 255), base.specular.Alpha() / 255.0, a.pageBg);

    a.toastBg      = CompositeOver(glassPigment, 0.92, base.bgDeep); // 与 bgDeep 同层但可辨
    a.textOnToast  = a.textOnAccent;                                  // 白字
    a.bubbleShadow = CompositeOver(a.userBubbleBottom, 0.30, a.pageBg);
    a.respShadow   = a.shadow;   // 0.45 黑，与既有浮层阴影同口径
    a.petShadow    = a.shadow;

    // 桌宠 artwork：与浅色档**共用** ApplyPetArtwork()（插画不随主题改色）。
    // 曾经把 petTie 绑到 brand，深色档于是变成 #74D38A —— 原型绿线恒为 #26813D。
    ApplyPetArtwork(a);

    // ---- 修正轮：语义边线逐档（原型 alpha 不合并）----
    a.borderAccentMid    = CompositeOver(a.accent, 0.35, a.pageBg);  // H161
    a.borderAccentStrong = CompositeOver(a.accent, 0.45, a.pageBg);  // H236
    a.borderDangerWeak   = CompositeOver(a.danger, 0.28, a.pageBg);  // H100
    a.borderWarnWeak     = CompositeOver(a.warn, 0.30, a.pageBg);    // H186
    a.borderWarnMid      = CompositeOver(a.warn, 0.35, a.pageBg);    // H128
    return a;
}

const AgentColours& AgentLight()
{
    static const AgentColours a = MakeAgentLight();
    return a;
}

const AgentColours& AgentDark()
{
    static const AgentColours a = MakeAgentDark();
    return a;
}

// ===========================================================================
// 状态与订阅
// ===========================================================================
constexpr const char* kModeKey = "Ui/Theme/Mode";

struct State
{
    Mode mode = Mode::System;
    int systemDarkOverride = -1;  ///< -1 = 真实探测；0 = 强制浅色；1 = 强制深色
    bool loaded = false;
    unsigned long nextId = 1;
    std::vector<std::pair<unsigned long, std::function<void()>>> subscribers;
};

State& S()
{
    static State s;
    return s;
}

/// Windows：读 HKCU\...\Themes\Personalize\AppsUseLightTheme（0 = 深色）
bool DetectSystemDark()
{
#ifdef __WXMSW__
    wxRegKey key(wxRegKey::HKCU,
                 wxS("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"));
    long value = 1;
    if (key.Exists() && key.Open(wxRegKey::Read)) {
        if (key.QueryValue(wxS("AppsUseLightTheme"), &value)) {
            return value == 0;
        }
    }
    return false;  // 探测失败 → 浅色（THE-006）
#else
    // Linux / 其他：本仓无可靠探测手段 → 按 SPEC THE-006 回退浅色
    return false;
#endif
}

void LoadModeIfNeeded()
{
    State& s = S();
    if (s.loaded) {
        return;
    }
    s.loaded = true;

    if (wxConfigBase* cfg = wxConfigBase::Get(false)) {
        wxString stored;
        if (cfg->Read(kModeKey, &stored) && !stored.empty()) {
            s.mode = ModeFromString(stored, Mode::System);
        }
    }
}

void PersistMode(Mode mode)
{
    if (wxConfigBase* cfg = wxConfigBase::Get(false)) {
        cfg->Write(kModeKey, ModeToString(mode));
        cfg->Flush();
    }
}

void Notify()
{
    // 复制一份回调再调用：回调内部可能解绑/新增订阅，避免迭代器失效
    std::vector<std::function<void()>> snapshot;
    snapshot.reserve(S().subscribers.size());
    for (const auto& entry : S().subscribers) {
        if (entry.second) {
            snapshot.push_back(entry.second);
        }
    }
    for (auto& cb : snapshot) {
        if (cb) {
            cb();
        }
    }
}

Mode ResolveMode(Mode mode)
{
    if (!kDarkModeEnabled) {
        return Mode::Light;  // DEC-11：深色档暂缓
    }
    if (mode != Mode::System) {
        return mode;
    }
    return IsSystemDark() ? Mode::Dark : Mode::Light;
}

}  // namespace

// ===========================================================================
// detail：订阅实现
// ===========================================================================
namespace detail {

unsigned long SubscribeImpl(std::function<void()> callback)
{
    State& s = S();
    const unsigned long id = s.nextId++;
    s.subscribers.emplace_back(id, std::move(callback));
    return id;
}

void UnsubscribeImpl(unsigned long id)
{
    if (id == 0) {
        return;
    }
    auto& subs = S().subscribers;
    for (auto it = subs.begin(); it != subs.end(); ++it) {
        if (it->first == id) {
            subs.erase(it);
            return;
        }
    }
}

}  // namespace detail

// ===========================================================================
// 公开 API
// ===========================================================================
bool IsSystemDark()
{
    if (!kDarkModeEnabled) {
        return false;  // DEC-11：深色档暂缓
    }
    if (S().systemDarkOverride >= 0) {
        return S().systemDarkOverride == 1;
    }
    return DetectSystemDark();
}

bool IsDarkModeEnabled()
{
    return kDarkModeEnabled;
}

Mode EffectiveMode()
{
    LoadModeIfNeeded();
    return ResolveMode(S().mode);
}

Mode GetMode()
{
    LoadModeIfNeeded();
    return S().mode;
}

void SetMode(Mode mode)
{
    LoadModeIfNeeded();
    S().mode = mode;
    PersistMode(mode);
    Notify();
}

void RefreshFromSystem()
{
    LoadModeIfNeeded();
    Notify();
}

wxString ModeToString(Mode mode)
{
    switch (mode) {
        case Mode::Light: return wxS("light");
        case Mode::Dark: return wxS("dark");
        case Mode::System: break;
    }
    return wxS("system");
}

Mode ModeFromString(const wxString& value, Mode fallback)
{
    const wxString normalized = value.Lower().Trim().Trim(false);
    if (normalized == wxS("light")) {
        return Mode::Light;
    }
    if (normalized == wxS("dark")) {
        return Mode::Dark;
    }
    if (normalized == wxS("system") || normalized == wxS("auto")) {
        return Mode::System;
    }
    return fallback;
}

const Colours& PaletteFor(Mode mode)
{
    return ResolveMode(mode) == Mode::Dark ? DarkPalette() : LightPalette();
}

const Colours& Palette()
{
    return PaletteFor(EffectiveMode());
}

const Colours& TerminalPalette()
{
    return DarkPalette();
}

wxColour CanvasSeedColour(const wxColour& seed, bool selected)
{
    const auto& p=Palette();
    if (selected) return p.accent;
    if (seed==wxColour(0,0,0) || seed==p.text1) return p.text2;
    if (seed==wxColour(255,255,255)) return p.canvas;
    return seed; // 用户数据中的语义/自定义色保持原值。
}

void PaintPageGradient(wxDC& dc, const wxSize& clientSize, const wxPoint& dcOrigin,
                       const wxRect& rect)
{
    const Colours& c = Palette();
    if (clientSize.x <= 0 || clientSize.y <= 0) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(c.bgPanel));
        dc.DrawRectangle(rect);
        return;
    }

    // 落在 55% 处的 bg-2 把渐变分成两段（wxDC::GradientFillLinear 每段只吃两个停靠色）。
    const int split = std::max(1, static_cast<int>(clientSize.y * 0.55));
    const int lower = std::max(1, clientSize.y - split);

    // 以顶层客户区为锚铺满，再裁剪到本区域 —— 分块绘制后接缝处颜色连续。
    // 坐标换算：DC 原点对应帧客户区的 (ox,oy)，所以帧客户区 [0,W]×[0,H] 映射到 DC 坐标
    // 是 [-ox, W-ox] —— 必须取负号，否则整块会偏出裁剪区（表现为一片黑）。
    wxDCClipper clip(dc, rect);
    dc.GradientFillLinear(wxRect(-dcOrigin.x, -dcOrigin.y, clientSize.x, split),
                          c.bgWindow, c.bgPanel, wxSOUTH);
    dc.GradientFillLinear(wxRect(-dcOrigin.x, -dcOrigin.y + split, clientSize.x, lower),
                          c.bgPanel, c.bgDeep, wxSOUTH);
}

void PaintCanvasBackdrop(wxDC& dc, const wxRect& rect, const wxWindow* window)
{
    const Colours& c = Palette();

    // 白底 --canvas
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(c.canvas));
    dc.DrawRectangle(rect);

    if (rect.width <= 0 || rect.height <= 0) return;

    const double dip = window != nullptr ? window->FromDIP(100) / 100.0 : 1.0;
    const int fine = std::max(1, static_cast<int>(20 * dip));    // CMP-041：20 DIP 细网格
    const int coarse = std::max(1, static_cast<int>(100 * dip)); // 100 DIP 粗网格

    dc.SetPen(wxPen(c.grid, 1));
    for (int x = rect.x + fine; x < rect.GetRight(); x += fine) dc.DrawLine(x, rect.y, x, rect.GetBottom());
    for (int y = rect.y + fine; y < rect.GetBottom(); y += fine) dc.DrawLine(rect.x, y, rect.GetRight(), y);

    dc.SetPen(wxPen(c.gridStrong, 1));
    for (int x = rect.x + coarse; x < rect.GetRight(); x += coarse) dc.DrawLine(x, rect.y, x, rect.GetBottom());
    for (int y = rect.y + coarse; y < rect.GetBottom(); y += coarse) dc.DrawLine(rect.x, y, rect.GetRight(), y);
}

const TeachingColours& Teaching()
{
    return EffectiveMode() == Mode::Dark ? TeachingDark() : TeachingLight();
}

// ===========================================================================
// Agent 扩展：公开的预合成口径（DEC-13 / THE-021）
//
// 必须定义在 UiTheme 命名空间（外层），不能只留在匿名命名空间：Agent 组件与
// 后续 T04/T05 都要按同一口径把 alpha 合成成实色，否则会各自发明第二套混合规则。
// ===========================================================================
wxColour CompositeOver(const wxColour& fg, double alpha, const wxColour& bg)
{
    const double a = alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha);
    auto mix = [a](int f, int b) {
        const double v = static_cast<double>(f) * a + static_cast<double>(b) * (1.0 - a);
        long r = static_cast<long>(v + 0.5);
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        return static_cast<unsigned char>(r);
    };
    return wxColour(mix(fg.Red(), bg.Red()), mix(fg.Green(), bg.Green()), mix(fg.Blue(), bg.Blue()));
}

const AgentColours& AgentColoursFor(Mode mode)
{
    return mode == Mode::Dark ? AgentDark() : AgentLight();
}

const AgentColours& AgentPalette()
{
    return AgentColoursFor(EffectiveMode());
}

wxFont UiFont(const wxWindow* window, TextSize size, Weight weight)
{
    const int dipHeight = static_cast<int>(size);
    // window 为空时（如 Art provider 构造期）仍按主显示器 DPI 换算，避免高 DPI 下字号偏小
    const int pixelHeight =
        window != nullptr ? window->FromDIP(dipHeight) : wxWindow::FromDIP(dipHeight, nullptr);

    // THE-016：继承系统 GUI 字体的具体 face。只有 Family 会留下空 face，
    // GDI+ CreateFont 直接使用 GetFaceName() 时会回退为错误的衬线字体。
    wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    font.SetPixelSize(wxSize(0, pixelHeight));
    font.SetWeight(static_cast<wxFontWeight>(weight));
    return font;
}

wxFont MonoFont(const wxWindow* window, TextSize size, Weight weight)
{
    const int dipHeight = static_cast<int>(size);
    const int pixelHeight =
        window != nullptr ? window->FromDIP(dipHeight) : wxWindow::FromDIP(dipHeight, nullptr);

    wxFontInfo info(wxSize(0, pixelHeight));
    info.Family(wxFONTFAMILY_TELETYPE);
#ifdef __WXMSW__
    // 同样为 GDI+ 提供可用的具体 face；只枚举一次，避免绘制时重复扫描字体。
    static const wxString face = [] {
        for (const auto* candidate : {wxS("Consolas"), wxS("Cascadia Mono")}) {
            if (wxFontEnumerator::IsValidFacename(candidate)) return wxString(candidate);
        }
        return wxSystemSettings::GetFont(wxSYS_ANSI_FIXED_FONT).GetFaceName();
    }();
    if (!face.empty()) info.FaceName(face);
#endif
    info.Weight(static_cast<int>(weight));
    return wxFont(info);
}

void SetSystemDarkOverrideForTest(int state)
{
    S().systemDarkOverride = state;
    Notify();
}

int CountUnsetTokens()
{
    int unset = 0;
    const Colours* palettes[] = { &LightPalette(), &DarkPalette() };
    for (const Colours* p : palettes) {
        const wxColour* fields[] = {
            &p->bgWindow, &p->bgPanel, &p->bgDeep,
            &p->surface, &p->surfaceStrong, &p->surfaceSunken,
            &p->stroke, &p->strokeStrong, &p->specular,
            &p->text1, &p->text2, &p->text3,
            &p->canvas, &p->grid, &p->gridStrong, &p->wire, &p->canvasShadow,
            &p->brand, &p->accent, &p->warn, &p->danger,
            &p->sel, &p->hover, &p->termBg,
            &p->surfaceOnWindow, &p->surfaceOnPanel, &p->surfaceOnCanvas,
            &p->strongOnPanel, &p->sunkenOnPanel, &p->sunkenOnCanvas,
            &p->strokeOnWindow, &p->strokeOnPanel,
            &p->strokeStrongOnWindow, &p->strokeStrongOnPanel,
            &p->selOnPanel, &p->hoverOnPanel, &p->focusRingOnPanel,
            &p->codeType, &p->codeComment,
            &p->glassEdge, &p->glassEdgeStrong, &p->glassEdgeSoft,
            &p->glassRim, &p->glassRing, &p->glassShadow, &p->glassBorder,
        };
        for (const wxColour* c : fields) {
            if (!c->IsOk()) {
                ++unset;
            }
        }
    }

    const TeachingColours* tch[] = { &TeachingLight(), &TeachingDark() };
    for (const TeachingColours* t : tch) {
        const wxColour* fields[] = {
            &t->guideBg, &t->guideText, &t->doneBg, &t->doneText,
            &t->hardwareBg, &t->hardwareText, &t->errorBg, &t->errorText,
            &t->stateOk, &t->stateWarn, &t->stateBad,
        };
        for (const wxColour* c : fields) {
            if (!c->IsOk()) {
                ++unset;
            }
        }
    }
    return unset;
}

// ===========================================================================
// 跨档 token 重映射（修 "深壳浅字 / 浅壳深字"）
//
// 背景：原生控件的前景色/背景色多为**构造期一次性写入**（`SetForegroundColour(Palette().textN)`），
//       主题切换后不会更新。T-140 夹具实测：深色档下主壳仅 53/134 个文字为深档 token，
//       其余 81 处仍是浅档 `#565A66`/`#1D1D1F` 画在 `#0A0B0E` 上 ⇒ 几乎不可读。
//
// 做法：把"仍等于**另一档** token 值"的颜色改成**当前档**的同一个 token。
//       与切换方向无关 ⇒ 幂等、无需记录上一档。
// ===========================================================================
namespace {

/// 需要跨档重映射的 token 成员表（一张表，浅/深两档各取一次同名成员）
constexpr wxColour Colours::* kRemapTokens[] = {
    &Colours::bgWindow,          &Colours::bgPanel,            &Colours::bgDeep,
    &Colours::surface,           &Colours::surfaceStrong,      &Colours::surfaceSunken,
    &Colours::stroke,            &Colours::strokeStrong,       &Colours::specular,
    &Colours::text1,             &Colours::text2,              &Colours::text3,
    &Colours::canvas,            &Colours::grid,               &Colours::gridStrong,
    &Colours::wire,              &Colours::canvasShadow,       &Colours::brand,
    &Colours::accent,            &Colours::warn,               &Colours::danger,
    &Colours::sel,               &Colours::hover,              &Colours::termBg,
    &Colours::surfaceOnWindow,   &Colours::surfaceOnPanel,     &Colours::surfaceOnCanvas,
    &Colours::strongOnPanel,     &Colours::sunkenOnCanvas,     &Colours::strokeOnWindow,
    &Colours::strokeOnPanel,     &Colours::strokeStrongOnWindow, &Colours::strokeStrongOnPanel,
    &Colours::selOnPanel,        &Colours::hoverOnPanel,       &Colours::focusRingOnPanel,
    &Colours::glassEdge,         &Colours::glassEdgeStrong,    &Colours::glassEdgeSoft,
    &Colours::glassRim,          &Colours::glassRing,          &Colours::glassShadow,
    &Colours::glassBorder,       &Colours::codeType,           &Colours::codeComment,
};

/// 在 `from` 档里找到等于 `colour` 的 token，返回 `to` 档的同名 token；找不到返回 invalid
wxColour MapAcrossBands(const Colours& from, const Colours& to, const wxColour& colour)
{
    for (const wxColour Colours::* member : kRemapTokens) {
        const wxColour& candidate = from.*member;
        const wxColour& mapped = to.*member;
        if (candidate.IsOk() && candidate == colour && !(mapped == colour)) {
            return mapped;
        }
    }
    return wxColour();
}

}  // namespace

unsigned RemapStaleTokenColours(wxWindow* root)
{
    if (root == nullptr) {
        return 0;
    }
    const Colours& now = Palette();
    const Colours& other = PaletteFor(EffectiveMode() == Mode::Dark ? Mode::Light : Mode::Dark);

    unsigned changed = 0;
    std::vector<wxWindow*> stack{ root };
    while (!stack.empty()) {
        wxWindow* current = stack.back();
        stack.pop_back();
        if (current == nullptr) {
            continue;
        }

        bool touched = false;

        const wxColour foreground = current->GetForegroundColour();
        if (foreground.IsOk()) {
            wxColour mapped = MapAcrossBands(other, now, foreground);
            if (!mapped.IsOk() && foreground == wxColour(0, 0, 0)) {
                // T-140 F-140-01：**平台默认文字色（纯黑、无 alpha）不是任何一档的 token**
                //（浅档 `canvasShadow` / 深档 `surfaceSunken` 虽含 RGB(0,0,0)，但带 alpha，
                // `operator==` 比 alpha 故不相等）→ 重映射原本看不见它，于是深色档下就是
                // "黑字深底"完全不可读。这里把它视为"未 token 化"，统一落到当档正文色。
                mapped = now.text1;
            }
            if (mapped.IsOk()) {
                current->SetForegroundColour(mapped);
                touched = true;
            }
        }

        const wxColour background = current->GetBackgroundColour();
        if (background.IsOk()) {
            const wxColour mapped = MapAcrossBands(other, now, background);
            if (mapped.IsOk()) {
                current->SetBackgroundColour(mapped);
                touched = true;
            }
        }

        if (touched) {
            ++changed;
            current->Refresh(false);
        }

        for (wxWindow* child : current->GetChildren()) {
            stack.push_back(child);
        }
    }
    return changed;
}

}  // namespace UiTheme