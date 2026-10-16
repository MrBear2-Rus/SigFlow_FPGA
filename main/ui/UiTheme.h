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
