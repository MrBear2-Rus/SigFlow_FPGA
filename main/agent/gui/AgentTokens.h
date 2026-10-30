#pragma once
//
// AgentTokens — Agent 局部设计 Token 的**只读门面**（AGUI-T04）
//
// 归属（DEC-13 / THE-001）：
//   * 唯一真源是 main/ui/UiTheme.*（AgentColours / AgentMetrics / AgentSpacing /
//     AgentTypography / AgentIcon / AgentOpacity / AgentGradient / AgentShadow /
//     AgentMotion）。本文件**不成为第二套调色板**：没有 hex 字面量、没有三参色值构造、
//     没有裸像素常量，只做「语义 → 组件」映射、单位换算与字体构造。
//   * 逐项设计溯源（index.html 行号、原值、浅/深推导、原生偏差）见
//     GUI/agent-gui/phase1/token-map.md。
//
// 单位口径（PLT-001）：
//   CSS px == DIP（设计稿按 1:1 逻辑像素写的，未按 DPR 折算，spec §13-4）。
//   交给 wx 之前必须经 FromDIP —— 用本文件的 CssPxToDip / UiFontPx 即可。
//   **CSS px ≠ 字体 point，但换算是定值**：1 CSS px ≡ 1/96 英寸，1 pt ≡ 1/72 英寸，
//   所以 pt = px × 72/96 = px × 0.75，**与显示器 DPI 无关**（12px ≡ 9pt，13px ≡ 9.75pt）。
//   错的写法是 `px*72/dpi`：那只适用于"物理像素"，13 CSS px 在 120 DPI 屏上仍是 9.75pt
//   而不是 7.8pt。wx 侧统一用**像素高度**渲染 UI 文本（与 UiTheme::UiFont 同口径，
//   THE-016）；point 只作记录（CssPxToPoint），不得拿 point 去 SetPointSize
//   （那会在高 DPI 下双重放大）。需要物理像素→point 时用 PhysicalPxToPoint()。
//
// 取整口径（分数档为什么必须保留 double）：
//   * 原型字号有 10 / 10.5 / 11 / 11.5 / 12 / 12.5 / 13 / 13.5px 八档；
//     半像素档是刻意的排版层次，不能被提前 static_cast 掉。
//   * 像素高度 = lround(cssPx × scale)，scale 由 wx 自己的 FromDIP 口径反推
//     （见 CssPxToDip 注释）：100% 下 10.5 → 11、11.5 → 12、12.5 → 13、13.5 → 14；
//     125% 下 10.5 → 13；150% 下 10.5 → 16。
//   * 亚像素排版不可用（GDI 整数像素），因此分数档在 100% DPI 下**允许 ±0.5px 差异**，
//     该差异已记录为"未验"，不得声称与浏览器逐像素一致。
//   * 边线同理：1 CSS px 在 125% 下仍是 1 像素（FromDIP 四舍五入），不产生 1.25px 细线。
//
// 有界性（公共 API 契约）：double 入参先判 std::isfinite、再夹紧到 [0, 1e6] CSS px，
//   然后才乘 scale 并 lround；NaN/±Inf/负数一律返回 0，超大值按 1e6 截断。
//   产品常量远小于该上限，这里只保证"任何输入都不会 UB 或产出错误量级"。
//
// 缓存：**不缓存**。UiTheme::EffectiveMode() 会随系统主题变化，DPI 也会随窗口在多显示器
//   之间迁移而变；每次调用只是取常量 + 一次乘法（O(1)），比维护
//   「mode + 缩放比例」双键缓存并处理失效更安全（THE-028）。
//

#include "ui/UiTheme.h"

#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/window.h>

#include <functional>

namespace agent::gui::layout {
struct Metrics;
}

namespace agent::gui::tokens {

/// 字重直接用 UiTheme 的档位，避免第二套枚举
using Weight = UiTheme::Weight;

// ---------------------------------------------------------------------------
// 调色板入口（转调 UiTheme，禁止本地再定义颜色）
// ---------------------------------------------------------------------------

/// 当前生效档的 Agent 调色板（= UiTheme::AgentPalette()）
const UiTheme::AgentColours& Colours();

/// 指定档的 Agent 调色板（= UiTheme::AgentColoursFor()）
const UiTheme::AgentColours& ColoursFor(UiTheme::Mode mode);

// ---------------------------------------------------------------------------
// 组件级映射
// ---------------------------------------------------------------------------

/// 组件档（对应 spec §10 组件体系；`ComponentToken()` 给稳定英文标识）
enum class Component
{
    Rail,         ///< 58px 窄功能侧栏（.rail / .rb）H309-325
    TopBar,       ///< 顶部菜单栏（.titlebar）H87-102
    Thread,       ///< 对话消息区（.aw-body / .msg / .resp）H208-228
    Composer,     ///< 输入与发送（.aw-compose）H254-261
    Card,         ///< 计划卡 / 思考链卡（.card / .chain）H229-253
    Float,        ///< 悬浮子窗（.float-layer .aw）H268-273
    Pet,          ///< 桌宠与气泡（.pet*）H275-307
    Badge,        ///< 模式标签 / 演示徽章 / wf-chip H100/H141/H199
    Button,       ///< 图标钮 / 等级钮 / 动作钮（.rb .wt .level .act）H179/H201/H250/H315
    Pill,         ///< 状态胶囊（.pill .chk .state-chip .chip）H159-164/H184-189/H227
    EvidenceRow,  ///< 证据行与课程引用（.evid / .evlist / .cite）H165-177/H223-225
    CodeBlock     ///< 等宽片段（.pid 10px 等宽 H177；原型无独立代码块 → 派生档）
};

/// 稳定英文 token（日志 / 自检用，不是界面文案）
const char* ComponentToken(Component component);

/// 组件级 Token。字段全部来自 UiTheme 的 Agent 扩展命名空间或 AgentColours；
/// 颜色以**成员指针**表达，取值时用 `SurfaceColour()` 等解析成当前档实色。
struct ComponentTokens
{
    int radiusDip;         ///< 圆角（DIP；0 = 直角容器，圆角在子控件上）
    int paddingXDip;       ///< 左右内距
    int paddingYDip;       ///< 上下内距（0 = 由 minHeight 居中，见备注）
    int gapDip;            ///< 内部元素间距
    int minHeightDip;      ///< 最小高度（0 = 由内容决定）
    int minWidthDip;       ///< 最小宽度（0 = 不限制）
    int maxWidthDip;       ///< 最大宽度（0 = 不限制）
    int maxHeightDip;      ///< 最大高度（0 = 不限制）
    double fontSizePx;     ///< 主字号（CSS px，可分数）
    Weight weight;         ///< 主字重（500/600 两档为主）
    double lineHeightRatio;///< 行高倍率（1.72 正文 / 1.55–1.60 辅助）
    int iconDip;           ///< 图标基准边长（0 = 本档无图标）
    int borderDip;         ///< 边线厚度（1 DIP 发丝线）
    int focusRingDip;      ///< 焦点环厚度（2 DIP；光晕 3 DIP 见 AgentMetrics::FocusGlow）
    bool mono;             ///< 使用等宽族（代码 / ID）
    double opacity;        ///< 相关透明度**权重**（实色已预合成，仅作溯源）

    wxColour UiTheme::AgentColours::*surface;  ///< 底色角色
    wxColour UiTheme::AgentColours::*text;     ///< 文字角色
    wxColour UiTheme::AgentColours::*border;   ///< 边线角色
    wxColour UiTheme::AgentColours::*emphasis; ///< 选中 / 进行中 / 强调角色
};

/// 组件 Token 表（只读；行数 == Component 枚举数，见编译期断言）
const ComponentTokens& Tokens(Component component);

/// 按当前主题档解析组件颜色（每次重算，不跨主题缓存）
wxColour SurfaceColour(Component component);
wxColour TextColour(Component component);
wxColour BorderColour(Component component);
wxColour EmphasisColour(Component component);

// ---------------------------------------------------------------------------
// 语义边线档（源码 alpha 逐档保留，不合并）
//
// 原型里同族边线的 alpha 并不统一（accent 有 .30/.35/.40/.45，warn 有 .30/.32/.35，
// danger 有 .28/.32）。5% 的通道差在深底上最多可达 13 个 8bit 台阶，肉眼可辨，
// 所以这里**逐档暴露**，由控件按真实状态取用；组件默认边框仍是 hairline。
// ---------------------------------------------------------------------------
enum class BorderRole
{
    Hairline,        ///< H49 --hairline（普通分隔/胶囊底边）
    AccentSoft,      ///< H141 accent .30（.wf-chip）
    AccentMid,       ///< H161 accent .35（.pill.blue）
    Accent,          ///< H126/H189/H204/H252/H318 accent .40（选中态）
    AccentStrong,    ///< H236 accent .45（.cstep .tick 1.4px 环）
    DangerWeak,      ///< H100 danger .28（.demo-badge）
    Danger,          ///< H160 danger .32（.pill.red）
    Ok,              ///< H162 green .32（.pill.green）
    WarnWeak,        ///< H186 warn .30（.chk.warn）
    Warn,            ///< H163 warn .32（.pill.amber）
    WarnMid,         ///< H128 warn .35（.step.recheck）
    MutedDashed      ///< H229 text3 .35（.chain 虚线）
};

/// 按当前主题档解析边线档（深色档为推导值，DEC-11 待验）
wxColour BorderColour(BorderRole role);

// ---------------------------------------------------------------------------
// 单位换算
// ---------------------------------------------------------------------------

/// CSS px → DIP 像素（整数入参直接走 wxWindow::FromDIP(int)，即 wx 自己的口径；
/// 0/负数返回 0）
int CssPxToDip(const wxWindow* window, int cssPx);

/// CSS px → DIP 像素（分数入参）。缩放比例**从 FromDIP 反推**：
///   scale = FromDIP(100) / 100.0  （wx 3.2 的 FromDIP 是
///   wxRescaleCoord(n).From(wxDisplay::GetStdPPI()).To(GetDPIHelper(w))，即
///   MulDiv 取整的 PPI 比值，**不等于** GetDPIScaleFactor()：端口上报 109 PPI 时
///   比值是 109/96≈1.135 而 scale factor 可能是 1.25）。
/// 再 lround(cssPx × scale)。入参有界：非有限值/负数 → 0，> 1e6 CSS px → 按 1e6。
int CssPxToDip(const wxWindow* window, double cssPx);

/// 物理像素 → CSS px / DIP（AGUI-T11）。CssPxToDip 的逆运算：
///   dip = lround(physicalPx / scale)，scale 同 CssPxToDip 的反推口径。
/// 断点判定必须用它把窗口客户尺寸换算成 DIP，**不得**拿物理像素直接比断点。
/// 入参有界：<=0 → 0；非有限/异常 scale → 原样返回；> 1e6 物理像素按 1e6。
int DipToCssPx(const wxWindow* window, int physicalPx);

/// AGUI-T11：把 UiTheme 的 Agent 度量/间距适配成纯布局策略的输入（仍是同一真源）。
layout::Metrics LayoutMetrics();

/// CSS px → 排版 point：**恒** cssPx × 0.75（= ×72/96）。
/// CSS px 是逻辑像素（1/96 in），与显示器 DPI 无关，因此本函数**不接 dpi 形参**：
/// 12px ≡ 9pt，13px ≡ 9.75pt，在任何缩放下都成立。
/// ⚠️ 只作记录 / 与设计稿对账：wx 侧 UI 文本一律用 UiFontPx() 的**像素高度**，
///    不要拿本值 SetPointSize。物理像素→point 请用 PhysicalPxToPoint()。
double CssPxToPoint(double cssPx);

/// **物理像素** → point（point = physicalPx × 72 / dpi）。
/// 只接受真正来自 GetDPI()/GetPPI() 的物理像素；不要把 CSS px 传进来
/// （那正是旧实现的错误：把逻辑 px 当物理 px 再除 DPI）。
/// dpi <= 0 时按 96 处理；入参有界同 CssPxToDip。
double PhysicalPxToPoint(double physicalPx, int dpi);

// ---------------------------------------------------------------------------
// 字体
// ---------------------------------------------------------------------------

/// UI 字体：系统 GUI 字体族 + 按 DIP 像素高度（小数档内部 lround）。
/// 不安装 SF Pro / PingFang；缺字由系统回退（PLT-002 / SYS-005）。
wxFont UiFontPx(const wxWindow* window, double cssPx, Weight weight = Weight::Normal);

/// 等宽字体：wxFONTFAMILY_TELETYPE（Windows → Consolas / Cascadia Mono）
wxFont MonoFontPx(const wxWindow* window, double cssPx, Weight weight = Weight::Normal);

/// 行高：lround(FromDIP(fontSizePx × ratio))；ratio <= 0 时按 1.0 处理
int LineHeightDip(const wxWindow* window, double fontSizePx, double ratio);

/// 行高（组件档版本）
int LineHeightDip(const wxWindow* window, const ComponentTokens& tokens);

// ---------------------------------------------------------------------------
// 文本折行（真实字体度量）
// ---------------------------------------------------------------------------

/// 按像素宽度折行后的文本与高度。
struct WrappedText
{
    wxString text;   ///< 已插入换行符，可直接交给 wxStaticText 显示
    int height = 0;  ///< 实际多行高度（DIP/物理像素，与 wx 一致）
};

/// 用真实字体度量逐字符折行（AGUI-T12 起为工作台与消息流共用实现）。
/// 不依赖控件当前宽度：首次布局时控件还是 0 宽，按 best-size 查询会把无空格中文
/// 折成一字一行，得到几十倍的高度并把后续内容挤出滚动区。measure 的是"真正画出来
/// 的多行文本"，因此存储高度与实际行数一致。
WrappedText WrapTextForWidth(wxWindow* owner, const wxFont& font, const wxString& source,
                             int width);

// ---------------------------------------------------------------------------
// 主题订阅
// ---------------------------------------------------------------------------

/// 透传 UiTheme::Subscribe（句柄析构即解绑）。调用方也可以直接用 UiTheme::Subscribe；
/// 本函数只是让 Agent 组件不必再包含 UiTheme.h 之外的主题实现细节。
UiTheme::Subscription SubscribeTheme(std::function<void()> callback);

// ---------------------------------------------------------------------------
// 编译期保证：关键 DIP 常量必须为正，且断点必须严格递减
// （只挑会影响布局正确性的值，不做逐常量机械断言）
// ---------------------------------------------------------------------------
static_assert(UiTheme::AgentMetrics::CardRadius > 0, "卡片圆角必须为正 DIP");
static_assert(UiTheme::AgentMetrics::ControlRadius > 0, "控件圆角必须为正 DIP");
static_assert(UiTheme::AgentMetrics::Hairline > 0, "发丝线厚度必须为正");
static_assert(UiTheme::AgentMetrics::FocusRing > 0, "焦点环厚度必须为正");
static_assert(UiTheme::AgentMetrics::ComposerHeight > 0, "输入行高度必须为正");
static_assert(UiTheme::AgentMetrics::ButtonHeight >= UiTheme::AgentMetrics::NarrowButtonHeight,
              "按钮高度不应小于窄屏档");
static_assert(UiTheme::AgentMetrics::ThreadMaxWidth > 0, "正文最大宽度必须为正");
static_assert(UiTheme::AgentMetrics::TitleBarHeight > 0, "顶栏高度必须为正");
static_assert(UiTheme::AgentMetrics::RailWidth > 0, "侧栏宽度必须为正");
static_assert(UiTheme::AgentMetrics::ColumnLeftWidth > 0, "左列宽必须为正");
static_assert(UiTheme::AgentMetrics::ColumnRightWidth > 0, "右列宽必须为正");
static_assert(UiTheme::AgentMetrics::FloatMaxWidth > 0, "浮窗最大宽必须为正");
static_assert(UiTheme::AgentMetrics::FloatMaxHeight > 0, "浮窗最大高必须为正");
static_assert(UiTheme::AgentMetrics::FloatInset > 0, "浮层边距必须为正");
static_assert(UiTheme::AgentMetrics::PetFigureSize > UiTheme::AgentMetrics::PetFigureSizeNarrow,
              "桌宠大图应大于窄屏档");
static_assert(UiTheme::AgentMetrics::WideBreakpoint > UiTheme::AgentMetrics::MediumBreakpoint &&
                  UiTheme::AgentMetrics::MediumBreakpoint > UiTheme::AgentMetrics::NarrowBreakpoint,
              "断点必须严格递减：宽 > 中 > 窄");
static_assert(UiTheme::AgentTypography::BodyPx > UiTheme::AgentTypography::SmallBodyPx &&
                  UiTheme::AgentTypography::SmallBodyPx > UiTheme::AgentTypography::TinyPx,
              "正文字号必须大于辅助字号");
static_assert(UiTheme::AgentTypography::LineHeightBody > 1.0, "行高倍率必须大于 1");
static_assert(UiTheme::AgentIcon::RailButton > UiTheme::AgentIcon::Evidence,
              "rail 图标应大于证据行图标");
static_assert(UiTheme::AgentOpacity::Glass > 0.0 && UiTheme::AgentOpacity::GlassStrong <= 1.0,
              "玻璃权重必须在 (0,1] 区间");
static_assert(UiTheme::AgentShadow::FloatBlur > UiTheme::AgentShadow::PopBlur,
              "浮层阴影应比弹出阴影更散（仅权重）");
// 修正轮：原型语义边线 alpha 是散值，六档必须互不相等（防止再被"归一化"成同一值）
static_assert(UiTheme::AgentOpacity::AccentBorderSoft != UiTheme::AgentOpacity::AccentBorder &&
                  UiTheme::AgentOpacity::AccentBorder != UiTheme::AgentOpacity::AccentBorderMid &&
                  UiTheme::AgentOpacity::AccentBorderMid !=
                      UiTheme::AgentOpacity::AccentBorderStrong,
              "accent 边线 .30/.35/.40/.45 必须逐档保留，不得合并");
static_assert(UiTheme::AgentOpacity::DangerBorderWeak != UiTheme::AgentOpacity::DangerBorder,
              "danger 边线 .28/.32 必须逐档保留");
static_assert(UiTheme::AgentOpacity::WarnBorderWeak != UiTheme::AgentOpacity::WarnBorder &&
                  UiTheme::AgentOpacity::WarnBorder != UiTheme::AgentOpacity::WarnBorderMid,
              "warn 边线 .30/.32/.35 必须逐档保留");
static_assert(UiTheme::AgentOpacity::MutedDashed > 0.0 &&
                  UiTheme::AgentOpacity::MutedDashed <= 1.0,
              "虚线边 alpha 必须在 (0,1]");

}  // namespace agent::gui::tokens
