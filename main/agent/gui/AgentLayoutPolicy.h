#pragma once
//
// AgentLayoutPolicy — AGUI-T11 纯布局策略（无 wxWidgets 依赖）
//
// 设计源：GUI/agent-gui/index.html H104–113（.workarea 三栏网格）、H332–361（三个断点，
// `.rail` 横排、`.float-layer .aw` 尺寸、`.pet`、`.rb` 34、`.brand span`/`.demo-badge`）、
// spec.md §3.2/§9/§11.2；design-review LAY-001/003/004/005/006；DEC-08（1180/960/560 按
// 客户端可用工作区 DIP 宽度，375 目标未取消）。
//
// 单位与调用口径（重要）：
//   * 本文件所有输入/输出都是 **DIP**（100% 缩放下的逻辑像素）。
//   * 宿主必须先把窗口客户尺寸换算成 DIP（`AgentTokens::DipToCssPx`），再把结果换算回
//     物理像素（`AgentTokens::CssPxToDip`）。**禁止**用物理像素直接和断点比较。
//   * 断点判定是闭区间：`<= wideMax` 进入 Compact、`<= mediumMax` 进入 Medium、
//     `<= narrowMax` 进入 Narrow；因此 1180/960/560 属于较小的一档，
//     1181/961/561 仍属于上一档。
//   * 数值真源仍是 UiTheme::AgentMetrics / AgentSpacing（由 `tokens::LayoutMetrics()`
//     适配成本结构）；本文件的 `Metrics` 只是**输入参数**，不另建第二套几何常量。
//
// 纯函数、可离屏单测：不做 DPI 缩放、不读系统设置、不创建窗口。

namespace agent::gui::layout {

enum class Breakpoint
{
    Wide = 0,   ///< > wideMax
    Compact,    ///< (mediumMax, wideMax]
    Medium,     ///< (narrowMax, mediumMax]
    Narrow,     ///< <= narrowMax
    Count
};

/// "wide"/"compact"/"medium"/"narrow"（日志/测试用，不是界面文案）。
const char* BreakpointToken(Breakpoint breakpoint);

/// 布局参数（DIP）。调用方用 `tokens::LayoutMetrics()` 填入，测试可自行构造。
struct Metrics
{
    int wideMax = 0;
    int mediumMax = 0;
    int narrowMax = 0;

    int guideWide = 0;
    int guideCompact = 0;
    int supportWide = 0;
    int supportCompact = 0;
    int pagePad = 0;
    int columnGap = 0;
    int mainMinWidth = 0;   ///< 低于此宽度时三栏降级为单列，避免主区横溢
    int mainMinHeight = 0;  ///< ≤960 单列时主区最小高度
    int sideMinHeight = 0;  ///< ≤960 单列时辅助区最小高度

    int railWidth = 0;
    int railButton = 0;
    int railButtonNarrow = 0;
    int railLogo = 0;
    int railLogoNarrow = 0;

    int threadMaxWidth = 0;  ///< 正文内容列最大宽度（居中）
    int threadPadMin = 0;

    int floatInsetWide = 0;
    int floatInsetCompact = 0;
    int floatMaxWidth = 0;
    int floatMaxHeight = 0;
    int floatMaxHeightCompact = 0;

    int petWidthWide = 0;
    int petWidthNarrow = 0;
    int petHeightWide = 0;
    int petHeightNarrow = 0;
    int petFigureWide = 0;
    int petFigureNarrow = 0;
    int petInsetWide = 0;
    int petInsetCompact = 0;
    int petLeftWide = 0;
    int petClamp = 0;
};

struct Box
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool IsEmpty() const { return width <= 0 || height <= 0; }
    bool operator==(const Box&) const = default;
};

/// 断点（DIP 宽度；<=0 视为 Narrow）。
Breakpoint ForWidth(int dipWidth, const Metrics& metrics);

/// 三栏/单列几何。`guideVisible` 为假（工作区模式）时 guide 宽度为 0 且不占 gap。
/// `stacked == true` 时 guide/main/support 都是整行宽度（按 main→guide→support 顺序排列）。
/// 宽度档只由 `tier` 决定：Wide（>1180）用 216/336；Compact/Medium/Narrow 用 200/312。
/// 最小宽度约束只作用于**弹性列**：center 模式是 main，dock 模式是 support
/// （dock 的 main 是设计固定的右侧窄列 312/336，允许小于 mainMinWidth）。
/// 计算始终满足：非 stacked 时 2*pagePad + guide + main + support + 2*gap == dipWidth
/// （工作模式下少一个 gap）；否则返回 stacked。
struct Columns
{
    bool stacked = false;
    int guide = 0;
    int main = 0;
    int support = 0;
    bool operator==(const Columns&) const = default;
};
Columns ColumnsFor(int dipWidth, bool guideVisible, bool docked, const Metrics& metrics);

/// 同上，但断点档由调用方给定：页面宽度决定档（rail 横向/纵向、浮窗/桌宠档位一致），
/// 网格输入宽度决定列宽（页面宽度减去 rail；函数内部再扣左右 pagePad）。这是 Workspace 使用的入口：
/// 保证"一个断点只算一次"，不会出现 rail 已横排而三栏仍按宽档排版。
Columns ColumnsForTier(int gridWidth, Breakpoint tier, bool guideVisible, bool docked,
                       const Metrics& metrics);

/// 正文内容列（.aw-body padding 14px max(16px,(100% - 760px)/2)）：
/// pad 随宽度增大居中，width 上限 threadMaxWidth。
struct ThreadColumn
{
    int pad = 0;
    int width = 0;
    bool operator==(const ThreadColumn&) const = default;
};
ThreadColumn ThreadColumnFor(int contentWidth, const Metrics& metrics);

// ---- 浮窗（.float-layer .aw）----
// >960 : top/right 16，宽 min(400, page-32)，高 min(596, page-32)
// ≤960 : top/right 8，宽 page-16，高 min(560, page-16)
// ≤560 : 铺满可用页（0,0,page）
bool FloatFullBleed(int pageWidth, const Metrics& metrics);
Box FloatBounds(int pageWidth, int pageHeight, const Metrics& metrics);
/// 拖动夹紧边距：铺满模式为 0（8 DIP 约束解除），其余为 Metrics::petClamp。
int FloatClamp(int pageWidth, const Metrics& metrics);

// ---- 桌宠（.pet）----
// >960 : 宽 248、高 240、left 24 / bottom 16
// ≤960 : left 8 / bottom 8（H348 `.pet{left:8px;bottom:8px}`，x 与 y 同步收）
// ≤560 : 宽 200、高 200（figure 140 由组件按窄档绘制）
Box PetBounds(int pageWidth, int pageHeight, const Metrics& metrics);
int PetFigureSize(int dipWidth, const Metrics& metrics);

// ---- rail ----
bool RailIsHorizontal(int dipWidth, const Metrics& metrics);  ///< <= mediumMax
int RailButtonSize(int dipWidth, const Metrics& metrics);
int RailLogoSize(int dipWidth, const Metrics& metrics);

}  // namespace agent::gui::layout
