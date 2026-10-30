// AgentLayoutPolicy.cpp — 纯布局策略实现（无 wxWidgets、无 IO、无状态、无异常）
//
// 口径：所有输入输出都是 DIP（100% 缩放下的逻辑像素）；断点判定为闭区间
// （宽度 <= 较大档的上界即降入较小档，因此 1180/960/560 属于较小档）。
// 所有减法一律经 std::max/std::min 夹紧，因此返回的几何永远不会为负。

#include "agent/gui/AgentLayoutPolicy.h"

#include <algorithm>

namespace agent::gui::layout {
namespace {

/// 去掉左右各一次 pad 后的可用宽度（逐次夹紧，避免大整数相减溢出成负）。
int InnerWidthOf(int width, int pad)
{
    const int safePad = std::max(0, pad);
    int inner         = std::max(0, width);
    inner             = std::max(0, inner - safePad);
    inner             = std::max(0, inner - safePad);
    return inner;
}

/// 列间总 gap：guide 可见时 2 个，否则 1 个（guide 隐藏时不占位也不占 gap）。
int GapTotalOf(int gap, bool guideVisible)
{
    const int safeGap = std::max(0, gap);
    int total         = safeGap;
    if (guideVisible)
    {
        total = std::max(0, total + safeGap);
    }
    return total;
}

}  // namespace

const char* BreakpointToken(Breakpoint breakpoint)
{
    switch (breakpoint)
    {
        case Breakpoint::Wide:
            return "wide";
        case Breakpoint::Compact:
            return "compact";
        case Breakpoint::Medium:
            return "medium";
        case Breakpoint::Narrow:
            return "narrow";
        default:
            return "narrow";  // Count 及越界值一律回退窄档
    }
}

Breakpoint ForWidth(int dipWidth, const Metrics& metrics)
{
    if (dipWidth <= 0)
    {
        return Breakpoint::Narrow;
    }
    if (dipWidth <= metrics.narrowMax)
    {
        return Breakpoint::Narrow;
    }
    if (dipWidth <= metrics.mediumMax)
    {
        return Breakpoint::Medium;
    }
    if (dipWidth <= metrics.wideMax)
    {
        return Breakpoint::Compact;
    }
    return Breakpoint::Wide;
}

Columns ColumnsForTier(int gridWidth, Breakpoint tier, bool guideVisible, bool docked,
                       const Metrics& metrics)
{
    const int gap     = std::max(0, metrics.columnGap);
    const int minMain = std::max(0, metrics.mainMinWidth);
    const int pw      = std::max(0, gridWidth);
    const int inner   = InnerWidthOf(pw, metrics.pagePad);

    // 单列：每一行都是整行宽度（guide 隐藏时不占位）。
    Columns stacked;
    stacked.stacked = true;
    if (inner <= 0)
    {
        return stacked;  // {true, 0, 0, 0}
    }
    stacked.guide   = guideVisible ? inner : 0;
    stacked.main    = inner;
    stacked.support = inner;

    // 三栏宽度取自所给档：只有 Wide 档用宽档列宽，Compact/Medium/Narrow 用紧凑列宽。
    const bool wideTier  = tier == Breakpoint::Wide;
    const int guideWidth = std::max(0, wideTier ? metrics.guideWide : metrics.guideCompact);
    const int sideWidth  = std::max(0, wideTier ? metrics.supportWide : metrics.supportCompact);

    // guide 可见时 = guide + gap + mainMin + gap + side；隐藏时 = mainMin + gap + side。
    // 停靠（docked）与居中（center）所需宽度相同，差别只在谁是弹性列。
    int needed = guideVisible ? guideWidth : 0;
    if (guideVisible)
    {
        needed = std::max(0, needed + gap);
    }
    needed = std::max(0, needed + minMain);
    needed = std::max(0, needed + gap);
    needed = std::max(0, needed + sideWidth);

    // 单列档（Medium/Narrow）无条件降级；否则再按所需宽度判断。
    const bool singleColumn = tier != Breakpoint::Wide && tier != Breakpoint::Compact;
    if (singleColumn || inner < needed)
    {
        return stacked;
    }

    // 弹性列 = inner - guide - side - gap 总数。
    int flexible = inner;
    if (guideVisible)
    {
        flexible = std::max(0, flexible - guideWidth);
    }
    flexible = std::max(0, flexible - sideWidth);
    flexible = std::max(0, flexible - GapTotalOf(gap, guideVisible));

    Columns columns;
    columns.stacked = false;
    columns.guide   = guideVisible ? guideWidth : 0;
    if (docked)
    {
        columns.main    = sideWidth;  // 停靠：主列用固定列宽，弹性列让给辅助列
        columns.support = flexible;
    }
    else
    {
        columns.main    = flexible;
        columns.support = sideWidth;
    }

    // 最小宽度约束只作用于弹性列：center 模式是 main，dock 模式是 support。
    // dock 的 main 是设计固定的右侧窄列（312/336），允许小于 mainMinWidth。
    const int flexibleColumn = docked ? columns.support : columns.main;
    if (flexibleColumn < minMain)
    {
        return stacked;  // 弹性列不足最小宽度时退回单列，避免横溢
    }
    return columns;
}

Columns ColumnsFor(int dipWidth, bool guideVisible, bool docked, const Metrics& metrics)
{
    return ColumnsForTier(dipWidth, ForWidth(dipWidth, metrics), guideVisible, docked, metrics);
}

ThreadColumn ThreadColumnFor(int contentWidth, const Metrics& metrics)
{
    ThreadColumn column;
    const int cw = std::max(0, contentWidth);
    if (cw <= 0)
    {
        return column;  // {0, 0}
    }

    const int maxWidth = std::max(0, metrics.threadMaxWidth);
    const int cap = maxWidth > 0 ? maxWidth : cw;
    // 居中留白向上取整，保证奇数宽度差时 width 不会比上限多 1（793 -> 17 + 759）。
    const int centered = cw > cap ? (cw - cap + 1) / 2 : 0;
    int pad            = std::max(std::max(0, metrics.threadPadMin), centered);
    pad                = std::min(pad, cw / 2);  // 左右各一半，绝不超过内容宽度

    column.pad   = pad;
    column.width = std::max(0, std::min(cap, cw - pad - pad));
    return column;
}

bool FloatFullBleed(int pageWidth, const Metrics& metrics)
{
    return pageWidth <= metrics.narrowMax;  // 含 pageWidth <= 0
}

Box FloatBounds(int pageWidth, int pageHeight, const Metrics& metrics)
{
    Box box;
    const int pw = std::max(0, pageWidth);
    const int ph = std::max(0, pageHeight);
    if (pw == 0 || ph == 0)
    {
        return box;  // 空页返回空盒
    }

    if (FloatFullBleed(pw, metrics))
    {
        box.width  = pw;
        box.height = ph;
        return box;  // {0, 0, pw, ph}
    }

    if (pw <= metrics.mediumMax)
    {
        const int inset  = std::max(0, metrics.floatInsetCompact);
        const int availW = std::max(0, pw - inset - inset);
        const int availH = std::max(0, ph - inset - inset);
        box.width  = availW;
        box.height = std::min(std::max(0, metrics.floatMaxHeightCompact), availH);
        box.x      = std::min(inset, std::max(0, pw - box.width));
        box.y      = std::min(inset, std::max(0, ph - box.height));
        return box;
    }

    const int inset  = std::max(0, metrics.floatInsetWide);
    const int availW = std::max(0, pw - inset - inset);
    const int availH = std::max(0, ph - inset - inset);
    box.width        = std::min(std::max(0, metrics.floatMaxWidth), availW);
    box.height       = std::min(std::max(0, metrics.floatMaxHeight), availH);
    box.x            = std::max(0, pw - box.width - inset);  // 右对齐后不越界
    box.y            = std::min(inset, std::max(0, ph - box.height));
    return box;
}

int FloatClamp(int pageWidth, const Metrics& metrics)
{
    if (FloatFullBleed(pageWidth, metrics))
    {
        return 0;  // 铺满模式解除 8 DIP 拖动约束
    }
    return std::max(0, metrics.petClamp);
}

Box PetBounds(int pageWidth, int pageHeight, const Metrics& metrics)
{
    Box box;
    const int pw = std::max(0, pageWidth);
    const int ph = std::max(0, pageHeight);
    if (pw == 0 || ph == 0)
    {
        return box;  // 空页返回空盒
    }

    const bool narrow  = pw <= metrics.narrowMax;
    const bool compact = pw <= metrics.mediumMax;
    const int clamp    = std::max(0, metrics.petClamp);

    int width  = std::max(0, narrow ? metrics.petWidthNarrow : metrics.petWidthWide);
    int height = std::max(0, narrow ? metrics.petHeightNarrow : metrics.petHeightWide);
    width      = std::min(width, std::max(0, pw - clamp - clamp));
    height     = std::min(height, std::max(0, ph - clamp - clamp));

    // left：Wide 档用 petLeftWide（24）贴设计稿左位；≤960（compact/narrow）与 y 同步收进
    // petInsetCompact（8，H348 `.pet{left:8px;bottom:8px}`）。
    int x = 0;
    if (compact)
    {
        x = std::min(std::max(0, metrics.petInsetCompact), std::max(0, pw - width));
    }
    else
    {
        const int left = std::max(0, metrics.petLeftWide);
        const int edge = std::max(clamp, std::max(0, pw - width - clamp));
        x              = std::min(left, edge);
    }

    const int bottomInset = compact ? metrics.petInsetCompact : metrics.petInsetWide;
    int y = std::max(clamp, ph - height - bottomInset);

    // 最后再压一次，保证整只桌宠留在页内。
    x = std::min(x, std::max(0, pw - width));
    y = std::min(y, std::max(0, ph - height));
    box.x      = std::max(0, x);
    box.y      = std::max(0, y);
    box.width  = width;
    box.height = height;
    return box;
}

int PetFigureSize(int dipWidth, const Metrics& metrics)
{
    const int size = dipWidth <= metrics.narrowMax ? metrics.petFigureNarrow : metrics.petFigureWide;
    return std::max(0, size);
}

bool RailIsHorizontal(int dipWidth, const Metrics& metrics)
{
    return dipWidth <= metrics.mediumMax;
}

int RailButtonSize(int dipWidth, const Metrics& metrics)
{
    const int size = dipWidth <= metrics.narrowMax ? metrics.railButtonNarrow : metrics.railButton;
    return std::max(0, size);
}

int RailLogoSize(int dipWidth, const Metrics& metrics)
{
    const int size = dipWidth <= metrics.mediumMax ? metrics.railLogoNarrow : metrics.railLogo;
    return std::max(0, size);
}

}  // namespace agent::gui::layout
