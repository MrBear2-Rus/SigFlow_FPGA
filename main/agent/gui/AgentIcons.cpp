//
// AgentIcons.cpp — AGUI-T06 图标与双球矢量资源的原生实现
//
// 设计源（逐行读取，行号即 icon-map.md 的溯源列）：
//   * GUI/agent-gui/index.html H397  rail 品牌缩略（viewBox 170，gB2/gR2）
//   * GUI/agent-gui/index.html H400–H436  rail 8 个功能入口 + 4 个形态入口（viewBox 24）
//   * GUI/agent-gui/index.html H496 桌宠角标气泡；H502 发送箭头（13px）
//   * GUI/agent-gui/index.html H521/H834 折叠箭头；H701/H845/H910 勾
//   * GUI/agent-gui/index.html H651 发送箭头（16px）；H625–H636 头部形态图标（15px，同几何）
//   * GUI/agent-gui/index.html H463–H494 桌宠双球（viewBox 170，地面椭圆/gB/gR/绿带/高光/眼睛/微笑）
//   * spec.md §4.2（rail 18×18 stroke 1.9）、§4.7（双球几何逐条）、§2.3（圆角/边线）、
//     §10（IconRail/IconButton 建议组件）
//   * design-review.md §3（桌宠 r34/r38、绿带宽 8、viewBox 170）与 C01（image.png 无绿带，
//     以当前内联 SVG 为源）、C14（原生与 CSS 的渲染差异需人工记录）
//   * phase1/token-map.md §9（图标尺寸来源 18/15/16/13/12/11/9/8，**没有 14px 图标**）、
//     §13-1..15（原生不可精确项）
//
// 实现要点
//   1. 单色图标 = 原型 viewBox 24 的**同一条 path 数据**，由本文件的极简 SVG path 解析器
//      （M/L/H/V/C/S/Q/T/A/Z，含相对指令与椭圆弧→三次贝塞尔）转成 wxGraphicsPath，
//      等比缩放（短边居中，**不拉伸**）后描边。因此几何比例与原型一致，不是"近似手绘"。
//   2. 双球 artwork = 固定 artwork 色 + 径向渐变 + 白高光 + 眼睛 + 微笑。渐变按 SVG
//      objectBoundingBox 单位换算：圆心 = 球包围盒左上 + (0.35,0.28)×边长，
//      半径 = 0.95×边长；停靠 0/0.55/1。
//   3. 颜色只有一个来源：
//      * 单色：调用方传入的 tint；
//      * artwork：`agent::gui::tokens::Colours()` 的 pet* 组（浅/深同值，token-map §13-14）。
//      本文件**不出现** hex / `wxColour(r,g,b)` 字面量（唯一例外见 PupilColour：
//      用 petGround 的 RGB 派生瞳色，因为原型瞳色 #1D1D1F 没有独立 token）。
//   4. 不读文件、不联网、不依赖 cwd：没有 wxFileSystem / wxImage::LoadFile / 资源查找。
//
// 已知差异（与原型/浏览器相比，不声称像素一致）：
//   * GDI+ 的径向渐变是"椭圆边界 + 中心点"的 PathGradientBrush 实现：SVG 的
//     cx/cy/r + 多档 stop 语义可以表达，但**球外区域不填充**（wx 源码 TODO 注释），
//     且插值曲线与 Skia 不同 → 色带过渡会有可见差异。
//   * SVG 的 radialGradient 没给 fx/fy，规范默认 fx=cx、fy=cy，因此这里是**同心**渐变，
//     不存在"focal 偏移无法表达"的问题；差异只在实现层的插值。
//   * CSS 的 `.pet-eye{animation:blink}`（H301–H303）、`bounce/stretch/dance/think`
//     （H295–H307）不在本文件实现：T06 只交静态矢量（动画属 T27）。
//   * 描边宽度按 SVG 语义随目标尺寸等比缩放（strokeCss × side/24），与 token-map §9
//     "按 1–2 DIP 近似"的记录一致；18 DIP 下 rail 描边 ≈1.42 DIP，12 DIP 箭头 ≈1.10 DIP。
//
#include "agent/gui/AgentIcons.h"

#include "agent/gui/AgentTokens.h"
#include "ui/UiTheme.h"

#include <wx/brush.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/pen.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>

namespace agent::gui::icons {
namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

constexpr double kPi = 3.14159265358979323846;

/// 单色图标的 viewBox 边长（原型 `<svg viewBox="0 0 24 24">`）
constexpr double kIconViewBox = 24.0;
/// 双球 artwork / 品牌缩略的 viewBox 边长（原型 `viewBox="0 0 170 170"`）
constexpr double kArtworkViewBox = 170.0;

/// RenderMonochrome 的尺寸上限（DIP）：超过则夹紧，避免误传造成巨量分配
constexpr int kMaxRenderDip = 512;

/// 桌宠小图档边长（H355 `@media (max-width:560px){.pet-figure,.pet-figure svg{width:140px}}`）
constexpr int kPetNarrowSideDip = 140;

// SVG radialGradient 的 objectBoundingBox 参数（H466–H475，gB/gR 与 gB2/gR2 同值）
constexpr double kGradCx = 0.35;
constexpr double kGradCy = 0.28;
constexpr double kGradR = 0.95;
constexpr double kGradMidStop = 0.55;

// SVG 里的 artwork alpha（H464 .07 / H482 .30 / H489 .26）
constexpr double kGroundAlpha = 0.07;
constexpr double kBlueHighlightAlpha = 0.30;
constexpr double kRedHighlightAlpha = 0.26;

// ---------------------------------------------------------------------------
// 形状与图标定义
// ---------------------------------------------------------------------------

enum class ShapeKind
{
    Path,    ///< SVG path 数据（viewBox 24）
    Circle,  ///< <circle cx cy r>
    Rect     ///< <rect x y w h rx>
};

struct Shape
{
    ShapeKind kind;
    const char* path;  ///< Path 专用；其余为 nullptr
    double a;          ///< Circle: cx；Rect: x
    double b;          ///< Circle: cy；Rect: y
    double c;          ///< Circle: r；Rect: w
    double d;          ///< Rect: h
    double e;          ///< Rect: rx（0 = 直角）
};

constexpr Shape PathShape(const char* d)
{
    return Shape{ShapeKind::Path, d, 0.0, 0.0, 0.0, 0.0, 0.0};
}
constexpr Shape CircleShape(double cx, double cy, double r)
{
    return Shape{ShapeKind::Circle, nullptr, cx, cy, r, 0.0, 0.0};
}
constexpr Shape RectShape(double x, double y, double w, double h, double rx)
{
    return Shape{ShapeKind::Rect, nullptr, x, y, w, h, rx};
}

struct IconDef
{
    IconId id;
    const Shape* shapes;
    int shapeCount;
    double strokeCss;  ///< 原型 viewBox 24 内的描边宽度（stroke-width）
};

// ---- 单色图标几何：path 数据与 index.html **逐字符相同** ----

constexpr Shape kNewAsk[] = {
    PathShape("M12 5v14M5 12h14"),  // H400
};

constexpr Shape kRailGuide[] = {
    CircleShape(5, 19, 2),            // H404
    CircleShape(12, 12, 2),
    CircleShape(19, 5, 2),
    PathShape("M6.6 17.4l3.8-3.8M13.6 10.4l3.8-3.8"),
};

constexpr Shape kRailIssue[] = {
    CircleShape(12, 12, 8),  // H407
    CircleShape(12, 12, 3),
};

constexpr Shape kRailEvidence[] = {
    PathShape("M9.5 3h5M10.5 3v5.2L5 17.8A2 2 0 0 0 6.8 21h10.4a2 2 0 0 0 1.8-3.2"
              "L13.5 8.2V3"),  // H410
};

constexpr Shape kRailPlan[] = {
    RectShape(5, 5, 14, 16, 2),                      // H413
    PathShape("M9 5V3h6v2M9 14l2 2 4-4.5"),
};

constexpr Shape kRailCourse[] = {
    PathShape("M5 4.5A2.5 2.5 0 0 1 7.5 2H19v17.5H7.5A2.5 2.5 0 0 0 5 22zM19 19.5H7.5"),  // H416
};

constexpr Shape kRailRecord[] = {
    PathShape("M7 3h10a1 1 0 0 1 1 1v17l-6-4.2L6 21V4a1 1 0 0 1 1-1z"),  // H419
};

constexpr Shape kRailChecker[] = {
    PathShape("M12 3l7 2.8V12c0 4.4-2.9 7.4-7 9-4.1-1.6-7-4.6-7-9V5.8z"),  // H422
    PathShape("M9 12l2 2 4-4.5"),
};

constexpr Shape kFormCenter[] = {
    PathShape("M21 12a8 8 0 0 1-11.5 7.2L4 20l1-4.5A8 8 0 1 1 21 12z"),  // H426 / H626
};

constexpr Shape kFormDock[] = {
    RectShape(3, 4, 18, 16, 2.5),  // H429 / H629
    PathShape("M15 4v16"),
};

constexpr Shape kFormFloat[] = {
    RectShape(3, 5, 18, 14, 2.5),  // H432 / H632
    PathShape("M3 9h18"),
};

constexpr Shape kFormPet[] = {
    RectShape(4, 7, 16, 12, 4),  // H435 / H635
    PathShape("M9 11h.01M15 11h.01M10 15h4M12 7V4"),
};

constexpr Shape kComposerSend[] = {
    PathShape("M5 12h14M13 6l6 6-6 6"),  // H651（16px）/ H502（13px）
};

constexpr Shape kCaretRight[] = {
    PathShape("M9 6l6 6-6 6"),  // H521 等 6 处；H834 思考链头同路径
};

constexpr Shape kCheckMark[] = {
    PathShape("M4 12.5l5 5L20 7"),  // H701 / H845 / H910
};

constexpr Shape kPetBadgeChat[] = {
    PathShape("M21 11.5a8.5 8.5 0 0 1-8.5 8.5c-1.5 0-2.9-.4-4.1-1L3 20l1.1-5.1A8.5 8.5 0 1 1 21 11.5z"),
};

// strokeCss 取该图标**主用法**的 stroke-width；其它用法见 icon-map 备注列
// （caret .chain-h 2.4、证据勾 2.6、桌宠送出 2.4 —— 换算后都在 1.0–1.6 DIP，差异已记录）。
constexpr IconDef kDefs[] = {
    {IconId::NewAsk, kNewAsk, static_cast<int>(std::size(kNewAsk)), 1.9},
    {IconId::RailGuide, kRailGuide, static_cast<int>(std::size(kRailGuide)), 1.9},
    {IconId::RailIssue, kRailIssue, static_cast<int>(std::size(kRailIssue)), 1.9},
    {IconId::RailEvidence, kRailEvidence, static_cast<int>(std::size(kRailEvidence)), 1.9},
    {IconId::RailPlan, kRailPlan, static_cast<int>(std::size(kRailPlan)), 1.9},
    {IconId::RailCourse, kRailCourse, static_cast<int>(std::size(kRailCourse)), 1.9},
    {IconId::RailRecord, kRailRecord, static_cast<int>(std::size(kRailRecord)), 1.9},
    {IconId::RailChecker, kRailChecker, static_cast<int>(std::size(kRailChecker)), 1.9},
    {IconId::FormCenter, kFormCenter, static_cast<int>(std::size(kFormCenter)), 1.9},
    {IconId::FormDock, kFormDock, static_cast<int>(std::size(kFormDock)), 1.9},
    {IconId::FormFloat, kFormFloat, static_cast<int>(std::size(kFormFloat)), 1.9},
    {IconId::FormPet, kFormPet, static_cast<int>(std::size(kFormPet)), 1.9},
    {IconId::ComposerSend, kComposerSend, static_cast<int>(std::size(kComposerSend)), 2.2},
    {IconId::CaretRight, kCaretRight, static_cast<int>(std::size(kCaretRight)), 2.2},
    {IconId::CheckMark, kCheckMark, static_cast<int>(std::size(kCheckMark)), 3.4},
    {IconId::PetBadgeChat, kPetBadgeChat, static_cast<int>(std::size(kPetBadgeChat)), 2.2},
};

static_assert(std::size(kDefs) == static_cast<std::size_t>(IconId::Count) - 3,
              "kDefs 只覆盖单色图标：Count 之前另有 BrandMark/PetArtwork 两个 artwork");

// ---------------------------------------------------------------------------
// 目录（稳定顺序 == IconId 顺序，Catalog()[i].id == IconId(i+1)）
// ---------------------------------------------------------------------------

constexpr IconInfo kCatalogData[] = {
    {IconId::NewAsk, "rail.new-ask", "index.html H400 .rb#newAsk svg (18px)", false, true, 18},
    {IconId::RailGuide, "rail.guide", "index.html H404 .rb#rGuide svg (18px)", false, true, 18},
    {IconId::RailIssue, "rail.issue", "index.html H407 .rb#rIssue svg (18px)", false, true, 18},
    {IconId::RailEvidence, "rail.evidence", "index.html H410 .rb#rEvid svg (18px)", false, true,
     18},
    {IconId::RailPlan, "rail.plan", "index.html H413 .rb#rPlan svg (18px)", false, true, 18},
    {IconId::RailCourse, "rail.course", "index.html H416 .rb#rCourse svg (18px)", false, true, 18},
    {IconId::RailRecord, "rail.record", "index.html H419 .rb#rRec svg (18px)", false, true, 18},
    {IconId::RailChecker, "rail.checker", "index.html H422 .rb#rChk svg (18px)", false, true, 18},
    {IconId::FormCenter, "form.center", "index.html H426 .rb#qCenter + H626 .wt#aCenter svg", false,
     true, 18},
    {IconId::FormDock, "form.dock", "index.html H429 .rb#qDock + H629 .wt#aDock svg", false, true,
     18},
    {IconId::FormFloat, "form.float", "index.html H432 .rb#qFloat + H632 .wt#aFloat svg", false,
     true, 18},
    {IconId::FormPet, "form.pet", "index.html H435 .rb#qPet + H635 .wt#aPet svg", false, true, 18},
    {IconId::ComposerSend, "composer.send",
     "index.html H651 .aw-compose .send svg (16px) + H502 .pet-mini .go svg (13px)", false, true,
     16},
    {IconId::CaretRight, "caret.right",
     "index.html H521/H534/H550/H568/H584/H605 .acc .caret (12px) + H834 .chain-h svg", false,
     true, 12},
    {IconId::CheckMark, "check.mark",
     "index.html H701 .step .dot svg (8px) + H845 .cstep .tick svg (9px) + H910 .evid svg (11px)",
     false, true, 11},
    {IconId::PetBadgeChat, "pet.badge-chat", "index.html H496 .pet-figure .badge svg (13px)",
     true, false, 13},
    {IconId::BrandMark, "brand.mark", "index.html H397 .rail-logo svg (36px, viewBox 170)", true,
     false, 36},
    {IconId::PetArtwork, "pet.artwork",
     "index.html H463-H494 #petSvg (viewBox 170; 170px @H283 / 140px @H355)", false, true, 170},
};

static_assert(std::size(kCatalogData) == static_cast<std::size_t>(IconId::Count) - 1,
              "目录必须覆盖除 Unknown 外的全部 IconId");
static_assert(std::size(kCatalogData) == std::size(kDefs) + 2,
              "每个 IconId 必须恰好有一条几何定义（单色 kDefs 或 artwork 专用实现）");

// 目录顺序与枚举顺序一致（逐条编译期核对，避免插入时错位）
constexpr bool CatalogOrderMatchesEnum()
{
    for (std::size_t i = 0; i < std::size(kCatalogData); ++i) {
        if (static_cast<std::size_t>(kCatalogData[i].id) != i + 1) {
            return false;
        }
    }
    return true;
}
static_assert(CatalogOrderMatchesEnum(), "Catalog 顺序必须与 IconId 定义顺序一致");
static_assert(std::size(kCatalogData) == 18, "T06 交付 18 个图标（8 rail + 4 形态 + 3 + 角标 + 2 artwork）");

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------

bool IsValidId(IconId id)
{
    const int raw = static_cast<int>(id);
    return raw > 0 && raw < static_cast<int>(IconId::Count);
}

bool IsArtwork(IconId id)
{
    return id == IconId::BrandMark || id == IconId::PetArtwork;
}

const IconDef* FindDef(IconId id)
{
    if (!IsValidId(id) || IsArtwork(id)) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(static_cast<int>(id) - 1);
    if (index >= std::size(kDefs) || kDefs[index].id != id) {
        return nullptr;
    }
    return &kDefs[index];
}

/// viewBox → gc 用户坐标：user = offset + scale × vb。
/// 正方形短边居中，因此非方形 box 也**只等比缩放、不拉伸**。
struct Frame
{
    double scale = 0.0;
    double ox = 0.0;
    double oy = 0.0;

    double X(double x) const { return ox + x * scale; }
    double Y(double y) const { return oy + y * scale; }
    double L(double length) const { return length * scale; }
};

Frame FitViewBox(const wxRect& box, double viewBoxSide)
{
    Frame frame;
    const double side = static_cast<double>(std::min(box.width, box.height));
    if (side <= 0.0 || viewBoxSide <= 0.0) {
        return frame;
    }
    frame.scale = side / viewBoxSide;
    frame.ox = static_cast<double>(box.x) + (static_cast<double>(box.width) - side) / 2.0;
    frame.oy = static_cast<double>(box.y) + (static_cast<double>(box.height) - side) / 2.0;
    return frame;
}

/// 保留 RGB、只改 alpha（原型里的 opacity -> RGBA）。
wxColour WithAlpha(const wxColour& base, double alpha)
{
    const double clamped = alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha);
    const int a = static_cast<int>(std::lround(clamped * 255.0));
    return wxColour(base.Red(), base.Green(), base.Blue(),
                    static_cast<unsigned char>(std::max(0, std::min(255, a))));
}

// ---------------------------------------------------------------------------
// 极简 SVG path 解析（只支持原型实际用到的指令，多也不行少也不行）
//
// 支持：M/m L/l H/h V/v C/c S/s Q/q T/t A/a Z/z（含隐式重复与相对坐标）。
// 弧线按 SVG 规范转成端点→圆心参数化，再拆成 ≤90° 的三次贝塞尔。
// 不支持：SVG2 的 B 弧语法、flags 连写（"0 0" 在我们数据里恒有分隔符）。
// ---------------------------------------------------------------------------

class SvgPathToGraphicsPath
{
public:
    SvgPathToGraphicsPath(wxGraphicsPath& path, const Frame& frame) : m_path(path), m_frame(frame)
    {
    }

    void Parse(const char* d)
    {
        if (d == nullptr) {
            return;
        }
        const char* p = d;
        char command = '\0';
        int guard = 0;
        while (*p != '\0' && guard++ < 100000) {
            SkipSeparators(p);
            if (*p == '\0') {
                break;
            }
            if (std::isalpha(static_cast<unsigned char>(*p)) != 0) {
                command = *p;
                ++p;
            } else if (command == '\0') {
                ++p;  // 非法前缀：跳过，不崩溃
                continue;
            } else if (command == 'M') {
                command = 'L';  // 规范：M 后续数值组按 L 处理
            } else if (command == 'm') {
                command = 'l';
            }
            Execute(command, p);
        }
    }

private:
    static void SkipSeparators(const char*& p)
    {
        while (*p != '\0' && (std::isspace(static_cast<unsigned char>(*p)) != 0 || *p == ',')) {
            ++p;
        }
    }

    bool ReadNumber(const char*& p, double& out)
    {
        SkipSeparators(p);
        if (*p == '\0') {
            return false;
        }
        char* end = nullptr;
        const double value = std::strtod(p, &end);
        if (end == p) {
            return false;
        }
        p = end;
        out = value;
        return true;
    }

    bool ReadFlag(const char*& p, bool& out)
    {
        double value = 0.0;
        if (!ReadNumber(p, value)) {
            return false;
        }
        out = value != 0.0;
        return true;
    }

    void Execute(char command, const char*& p)
    {
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));

        switch (upper) {
            case 'M': {
                double x = 0.0, y = 0.0;
                if (!ReadNumber(p, x) || !ReadNumber(p, y)) {
                    return;
                }
                MoveTo(relative ? m_curX + x : x, relative ? m_curY + y : y);
                m_prevCommand = 'M';
                return;
            }
            case 'L': {
                double x = 0.0, y = 0.0;
                if (!ReadNumber(p, x) || !ReadNumber(p, y)) {
                    return;
                }
                LineTo(relative ? m_curX + x : x, relative ? m_curY + y : y);
                m_prevCommand = 'L';
                return;
            }
            case 'H': {
                double x = 0.0;
                if (!ReadNumber(p, x)) {
                    return;
                }
                LineTo(relative ? m_curX + x : x, m_curY);
                m_prevCommand = 'L';
                return;
            }
            case 'V': {
                double y = 0.0;
                if (!ReadNumber(p, y)) {
                    return;
                }
                LineTo(m_curX, relative ? m_curY + y : y);
                m_prevCommand = 'L';
                return;
            }
            case 'C': {
                double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
                if (!ReadNumber(p, x1) || !ReadNumber(p, y1) || !ReadNumber(p, x2) ||
                    !ReadNumber(p, y2) || !ReadNumber(p, x) || !ReadNumber(p, y)) {
                    return;
                }
                const double bx = relative ? m_curX : 0.0;
                const double by = relative ? m_curY : 0.0;
                CubicTo(bx + x1, by + y1, bx + x2, by + y2, bx + x, by + y);
                m_prevCommand = 'C';
                return;
            }
            case 'S': {
                double x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
                if (!ReadNumber(p, x2) || !ReadNumber(p, y2) || !ReadNumber(p, x) ||
                    !ReadNumber(p, y)) {
                    return;
                }
                const double bx = relative ? m_curX : 0.0;
                const double by = relative ? m_curY : 0.0;
                double c1x = m_curX;
                double c1y = m_curY;
                if (m_prevCommand == 'C' || m_prevCommand == 'S') {
                    c1x = 2.0 * m_curX - m_lastCubicCx;
                    c1y = 2.0 * m_curY - m_lastCubicCy;
                }
                CubicTo(c1x, c1y, bx + x2, by + y2, bx + x, by + y);
                m_prevCommand = 'S';
                return;
            }
            case 'Q': {
                double x1 = 0.0, y1 = 0.0, x = 0.0, y = 0.0;
                if (!ReadNumber(p, x1) || !ReadNumber(p, y1) || !ReadNumber(p, x) ||
                    !ReadNumber(p, y)) {
                    return;
                }
                const double bx = relative ? m_curX : 0.0;
                const double by = relative ? m_curY : 0.0;
                QuadTo(bx + x1, by + y1, bx + x, by + y);
                m_prevCommand = 'Q';
                return;
            }
            case 'T': {
                double x = 0.0, y = 0.0;
                if (!ReadNumber(p, x) || !ReadNumber(p, y)) {
                    return;
                }
                const double bx = relative ? m_curX : 0.0;
                const double by = relative ? m_curY : 0.0;
                double qx = m_curX;
                double qy = m_curY;
                if (m_prevCommand == 'Q' || m_prevCommand == 'T') {
                    qx = 2.0 * m_curX - m_lastQuadCx;
                    qy = 2.0 * m_curY - m_lastQuadCy;
                }
                QuadTo(qx, qy, bx + x, by + y);
                m_prevCommand = 'T';
                return;
            }
            case 'A': {
                double rx = 0.0, ry = 0.0, rotation = 0.0, x = 0.0, y = 0.0;
                bool largeArc = false;
                bool sweep = false;
                if (!ReadNumber(p, rx) || !ReadNumber(p, ry) || !ReadNumber(p, rotation) ||
                    !ReadFlag(p, largeArc) || !ReadFlag(p, sweep) || !ReadNumber(p, x) ||
                    !ReadNumber(p, y)) {
                    return;
                }
                ArcTo(rx, ry, rotation, largeArc, sweep, relative ? m_curX + x : x,
                      relative ? m_curY + y : y);
                m_prevCommand = 'A';
                return;
            }
            case 'Z': {
                ClosePath();
                m_prevCommand = 'Z';
                return;
            }
            default:
                return;  // 未知指令：忽略剩余（不崩溃）
        }
    }

    void MoveTo(double x, double y)
    {
        m_path.MoveToPoint(m_frame.X(x), m_frame.Y(y));
        m_curX = x;
        m_curY = y;
        m_startX = x;
        m_startY = y;
    }

    void LineTo(double x, double y)
    {
        m_path.AddLineToPoint(m_frame.X(x), m_frame.Y(y));
        m_curX = x;
        m_curY = y;
    }

    void CubicTo(double x1, double y1, double x2, double y2, double x, double y)
    {
        m_path.AddCurveToPoint(m_frame.X(x1), m_frame.Y(y1), m_frame.X(x2), m_frame.Y(y2),
                               m_frame.X(x), m_frame.Y(y));
        m_lastCubicCx = x2;
        m_lastCubicCy = y2;
        m_curX = x;
        m_curY = y;
    }

    void QuadTo(double x1, double y1, double x, double y)
    {
        // 二次 → 三次：C1 = P0 + 2/3(Q−P0)，C2 = P1 + 2/3(Q−P1)
        const double c1x = m_curX + 2.0 / 3.0 * (x1 - m_curX);
        const double c1y = m_curY + 2.0 / 3.0 * (y1 - m_curY);
        const double c2x = x + 2.0 / 3.0 * (x1 - x);
        const double c2y = y + 2.0 / 3.0 * (y1 - y);
        m_path.AddCurveToPoint(m_frame.X(c1x), m_frame.Y(c1y), m_frame.X(c2x), m_frame.Y(c2y),
                               m_frame.X(x), m_frame.Y(y));
        m_lastQuadCx = x1;
        m_lastQuadCy = y1;
        m_curX = x;
        m_curY = y;
    }

    void ClosePath()
    {
        m_path.CloseSubpath();
        m_curX = m_startX;
        m_curY = m_startY;
    }

    /// SVG 椭圆弧（端点参数化）→ 若干三次贝塞尔。
    void ArcTo(double rx, double ry, double rotationDeg, bool largeArc, bool sweep, double x2,
               double y2)
    {
        if (rx == 0.0 || ry == 0.0) {
            LineTo(x2, y2);
            return;
        }
        rx = std::fabs(rx);
        ry = std::fabs(ry);
        const double x1 = m_curX;
        const double y1 = m_curY;
        if (std::fabs(x1 - x2) < 1e-12 && std::fabs(y1 - y2) < 1e-12) {
            return;  // 规范：起终点重合则整段弧忽略
        }
        const double phi = rotationDeg * kPi / 180.0;
        const double cosPhi = std::cos(phi);
        const double sinPhi = std::sin(phi);

        const double dx2 = (x1 - x2) / 2.0;
        const double dy2 = (y1 - y2) / 2.0;
        const double x1p = cosPhi * dx2 + sinPhi * dy2;
        const double y1p = -sinPhi * dx2 + cosPhi * dy2;

        double rx2 = rx * rx;
        double ry2 = ry * ry;
        const double x1p2 = x1p * x1p;
        const double y1p2 = y1p * y1p;
        const double lambda = x1p2 / rx2 + y1p2 / ry2;
        if (lambda > 1.0) {
            const double k = std::sqrt(lambda);
            rx *= k;
            ry *= k;
            rx2 = rx * rx;
            ry2 = ry * ry;
        }

        double numerator = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
        if (numerator < 0.0) {
            numerator = 0.0;
        }
        const double denominator = rx2 * y1p2 + ry2 * x1p2;
        const double factor =
            (largeArc == sweep ? -1.0 : 1.0) *
            std::sqrt(denominator > 0.0 ? numerator / denominator : 0.0);
        const double cxp = factor * (rx * y1p / ry);
        const double cyp = factor * (-ry * x1p / rx);
        const double cx = cosPhi * cxp - sinPhi * cyp + (x1 + x2) / 2.0;
        const double cy = sinPhi * cxp + cosPhi * cyp + (y1 + y2) / 2.0;

        const double ux = (x1p - cxp) / rx;
        const double uy = (y1p - cyp) / ry;
        const double vx = (-x1p - cxp) / rx;
        const double vy = (-y1p - cyp) / ry;

        const auto signedAngle = [](double ax, double ay, double bx, double by) {
            const double dot = ax * bx + ay * by;
            const double len = std::sqrt((ax * ax + ay * ay) * (bx * bx + by * by));
            double angle = 0.0;
            if (len > 0.0) {
                double ratio = dot / len;
                ratio = ratio < -1.0 ? -1.0 : (ratio > 1.0 ? 1.0 : ratio);
                angle = std::acos(ratio);
            }
            if (ax * by - ay * bx < 0.0) {
                angle = -angle;
            }
            return angle;
        };

        const double theta1 = signedAngle(1.0, 0.0, ux, uy);
        double deltaTheta = signedAngle(ux, uy, vx, vy);
        if (!sweep && deltaTheta > 0.0) {
            deltaTheta -= 2.0 * kPi;
        } else if (sweep && deltaTheta < 0.0) {
            deltaTheta += 2.0 * kPi;
        }

        const int segments =
            std::max(1, static_cast<int>(std::ceil(std::fabs(deltaTheta) / (kPi / 2.0))));
        const double step = deltaTheta / static_cast<double>(segments);
        const double handle = 4.0 / 3.0 * std::tan(step / 4.0);

        double theta = theta1;
        for (int i = 0; i < segments; ++i) {
            const double thetaNext = theta + step;
            const double cosA = std::cos(theta);
            const double sinA = std::sin(theta);
            const double cosB = std::cos(thetaNext);
            const double sinB = std::sin(thetaNext);

            const double startX = cx + rx * cosPhi * cosA - ry * sinPhi * sinA;
            const double startY = cy + rx * sinPhi * cosA + ry * cosPhi * sinA;
            const double endX = cx + rx * cosPhi * cosB - ry * sinPhi * sinB;
            const double endY = cy + rx * sinPhi * cosB + ry * cosPhi * sinB;

            const double dStartX = -rx * cosPhi * sinA - ry * sinPhi * cosA;
            const double dStartY = -rx * sinPhi * sinA + ry * cosPhi * cosA;
            const double dEndX = -rx * cosPhi * sinB - ry * sinPhi * cosB;
            const double dEndY = -rx * sinPhi * sinB + ry * cosPhi * cosB;

            const bool last = (i == segments - 1);
            const double finalX = last ? x2 : endX;
            const double finalY = last ? y2 : endY;

            m_path.AddCurveToPoint(m_frame.X(startX + handle * dStartX),
                                   m_frame.Y(startY + handle * dStartY),
                                   m_frame.X(endX - handle * dEndX),
                                   m_frame.Y(endY - handle * dEndY), m_frame.X(finalX),
                                   m_frame.Y(finalY));
            theta = thetaNext;
        }
        m_curX = x2;
        m_curY = y2;
    }

    wxGraphicsPath& m_path;
    Frame m_frame;
    double m_curX = 0.0;
    double m_curY = 0.0;
    double m_startX = 0.0;
    double m_startY = 0.0;
    double m_lastCubicCx = 0.0;
    double m_lastCubicCy = 0.0;
    double m_lastQuadCx = 0.0;
    double m_lastQuadCy = 0.0;
    char m_prevCommand = '\0';
};

// ---------------------------------------------------------------------------
// 绘制辅助
// ---------------------------------------------------------------------------

void StrokePathData(wxGraphicsContext& gc, const Frame& frame, const char* data)
{
    wxGraphicsPath path = gc.CreatePath();
    SvgPathToGraphicsPath parser(path, frame);
    parser.Parse(data);
    gc.StrokePath(path);
}

/// 单色（描边）图标：fill:none + stroke:tint，圆头圆角（对应 CSS
/// `.rb svg{fill:none;stroke:currentColor;stroke-linecap:round;stroke-linejoin:round}`）。
bool StrokeMonochrome(wxGraphicsContext& gc, const IconDef& def, const wxRect& box,
                      const wxColour& tint)
{
    const Frame frame = FitViewBox(box, kIconViewBox);
    if (frame.scale <= 0.0) {
        return false;
    }
    const double strokeWidth = std::max(0.5, def.strokeCss * frame.scale);
    gc.SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc.SetPen(gc.CreatePen(
        wxGraphicsPenInfo(tint).Width(strokeWidth).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);

    for (int i = 0; i < def.shapeCount; ++i) {
        const Shape& shape = def.shapes[i];
        switch (shape.kind) {
            case ShapeKind::Path:
                StrokePathData(gc, frame, shape.path);
                break;
            case ShapeKind::Circle: {
                const double r = frame.L(shape.c);
                gc.DrawEllipse(frame.X(shape.a) - r, frame.Y(shape.b) - r, 2.0 * r, 2.0 * r);
                break;
            }
            case ShapeKind::Rect: {
                const double x = frame.X(shape.a);
                const double y = frame.Y(shape.b);
                const double w = frame.L(shape.c);
                const double h = frame.L(shape.d);
                if (shape.e > 0.0) {
                    gc.DrawRoundedRectangle(x, y, w, h, frame.L(shape.e));
                } else {
                    gc.DrawRectangle(x, y, w, h);
                }
                break;
            }
        }
    }
    return true;
}

void FillEllipse(wxGraphicsContext& gc, const Frame& frame, double x, double y, double w, double h,
                 const wxColour& colour)
{
    gc.SetBrush(gc.CreateBrush(wxBrush(colour)));
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.DrawEllipse(frame.X(x), frame.Y(y), frame.L(w), frame.L(h));
}

void FillCircle(wxGraphicsContext& gc, const Frame& frame, double cx, double cy, double r,
                const wxColour& colour)
{
    FillEllipse(gc, frame, cx - r, cy - r, 2.0 * r, 2.0 * r, colour);
}

/// 球体径向渐变（SVG radialGradient 的 objectBoundingBox 口径）。
/// bboxX/bboxY/bboxSide 是该 <circle> 的包围盒（r×2 的正方形）。
void FillRadialBall(wxGraphicsContext& gc, const Frame& frame, double cx, double cy, double r,
                    double bboxX, double bboxY, double bboxSide, const wxColour& top,
                    const wxColour& mid, const wxColour& bottom)
{
    const double gx = bboxX + kGradCx * bboxSide;
    const double gy = bboxY + kGradCy * bboxSide;
    const double gr = kGradR * bboxSide;

    wxGraphicsGradientStops stops(top, bottom);
    stops.Add(mid, kGradMidStop);

    // SVG 未给 fx/fy → 规范默认 fx=cx、fy=cy，即同心渐变；这里 start==end。
    wxGraphicsBrush brush = gc.CreateRadialGradientBrush(
        frame.X(gx), frame.Y(gy), frame.X(gx), frame.Y(gy), frame.L(gr), stops);
    gc.SetBrush(brush);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.DrawEllipse(frame.X(cx - r), frame.Y(cy - r), frame.L(2.0 * r), frame.L(2.0 * r));
}

/// 瞳色：原型 #1D1D1F（H483/H490）没有独立 token。petGround 的 RGB 恰为同值且属于
/// artwork 固定色，因此从这里派生，避免在实现里写色值字面量。
wxColour PupilColour()
{
    const wxColour& ground = agent::gui::tokens::Colours().petGround;
    return wxColour(ground.Red(), ground.Green(), ground.Blue(), 255);
}

/// 一只眼睛：白眼球 + 瞳（原型 `.pet-eye` 的静态态；眨眼动画见 H301–H303，T06 不做）。
void DrawEye(wxGraphicsContext& gc, const Frame& frame, double eyeCx, double eyeCy, double eyeR,
             double pupilCx, double pupilCy, double pupilR, const wxColour& white,
             const wxColour& pupil)
{
    FillCircle(gc, frame, eyeCx, eyeCy, eyeR, white);
    FillCircle(gc, frame, pupilCx, pupilCy, pupilR, pupil);
}

// ---------------------------------------------------------------------------
// artwork：桌宠双球（H463–H494）
// ---------------------------------------------------------------------------

bool DrawPet(wxGraphicsContext& gc, const wxRect& box, bool narrow)
{
    wxRect target = box;
    if (narrow) {
        const int side = std::min(std::min(box.width, box.height), kPetNarrowSideDip);
        target.x = box.x + (box.width - side) / 2;
        target.y = box.y + (box.height - side) / 2;
        target.width = side;
        target.height = side;
    }
    const Frame frame = FitViewBox(target, kArtworkViewBox);
    if (frame.scale <= 0.0) {
        return false;
    }

    const UiTheme::AgentColours& colours = agent::gui::tokens::Colours();
    const wxColour pupil = PupilColour();
    const wxColour highlight = colours.petHighlight;

    gc.SetAntialiasMode(wxANTIALIAS_DEFAULT);

    // 1) 地面椭圆 H464：cx106 cy150 rx60 ry8（petGround 保留 7% alpha，必须走 gc）
    FillEllipse(gc, frame, 106.0 - 60.0, 150.0 - 8.0, 120.0, 16.0,
                WithAlpha(colours.petGround, kGroundAlpha));

    // 2) 绿带 H478：两端在球心 (44,66)→(124,106)，宽 8、圆头，压在球体之下
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colours.petTie).Width(frame.L(8.0)).Cap(wxCAP_ROUND)));
    gc.StrokeLine(frame.X(44.0), frame.Y(66.0), frame.X(124.0), frame.Y(106.0));

    // 3) 蓝球 H480–H485：圆心 (44,66) r34；包围盒 (10,32) 边长 68
    FillRadialBall(gc, frame, 44.0, 66.0, 34.0, 10.0, 32.0, 68.0, colours.petBlueTop,
                   colours.petBlueMid, colours.petBlueBottom);
    FillCircle(gc, frame, 32.0, 54.0, 8.0, WithAlpha(highlight, kBlueHighlightAlpha));
    DrawEye(gc, frame, 34.0, 63.0, 5.0, 34.0, 64.0, 2.5, highlight, pupil);
    DrawEye(gc, frame, 54.0, 63.0, 5.0, 54.0, 64.0, 2.5, highlight, pupil);

    // 4) 红球 H487–H493：圆心 (124,106) r38；包围盒 (86,68) 边长 76
    FillRadialBall(gc, frame, 124.0, 106.0, 38.0, 86.0, 68.0, 76.0, colours.petRedTop,
                   colours.petRedMid, colours.petRedBottom);
    FillCircle(gc, frame, 110.0, 90.0, 9.0, WithAlpha(highlight, kRedHighlightAlpha));
    DrawEye(gc, frame, 114.0, 102.0, 5.5, 114.0, 103.0, 2.8, highlight, pupil);
    DrawEye(gc, frame, 136.0, 102.0, 5.5, 136.0, 103.0, 2.8, highlight, pupil);

    // 5) 微笑 H492：M116 120 q9 9 19 0，白 4、圆头、无填充
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(highlight).Width(frame.L(4.0)).Cap(wxCAP_ROUND)));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    StrokePathData(gc, frame, "M116 120 q9 9 19 0");

    return true;
}

// ---------------------------------------------------------------------------
// artwork：rail 品牌缩略（H397，36 DIP）
//
// 与桌宠**不同的几何**（不得互相复用）：蓝球 (60,70) r34、红球 (116,106) r40、
// 绿带宽 14、无地面椭圆/眼睛/微笑，只有一枚 (49,59) r7 的高光。
// ---------------------------------------------------------------------------

bool DrawBrand(wxGraphicsContext& gc, const wxRect& box)
{
    const Frame frame = FitViewBox(box, kArtworkViewBox);
    if (frame.scale <= 0.0) {
        return false;
    }
    const UiTheme::AgentColours& colours = agent::gui::tokens::Colours();
    gc.SetAntialiasMode(wxANTIALIAS_DEFAULT);

    // 绿线 (60,70)→(116,106)，宽 14，圆头
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colours.petTie).Width(frame.L(14.0)).Cap(wxCAP_ROUND)));
    gc.StrokeLine(frame.X(60.0), frame.Y(70.0), frame.X(116.0), frame.Y(106.0));

    // 蓝球 (60,70) r34 → 包围盒 (26,36) 边长 68
    FillRadialBall(gc, frame, 60.0, 70.0, 34.0, 26.0, 36.0, 68.0, colours.petBlueTop,
                   colours.petBlueMid, colours.petBlueBottom);
    // 红球 (116,106) r40 → 包围盒 (76,66) 边长 80
    FillRadialBall(gc, frame, 116.0, 106.0, 40.0, 76.0, 66.0, 80.0, colours.petRedTop,
                   colours.petRedMid, colours.petRedBottom);

    // 高光 (49,59) r7，白 30%
    FillCircle(gc, frame, 49.0, 59.0, 7.0, WithAlpha(colours.petHighlight, kBlueHighlightAlpha));
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------

const std::vector<IconInfo>& Catalog()
{
    static const std::vector<IconInfo> catalog(std::begin(kCatalogData), std::end(kCatalogData));
    return catalog;
}

const IconInfo* Find(IconId id)
{
    if (!IsValidId(id)) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(static_cast<int>(id) - 1);
    const std::vector<IconInfo>& catalog = Catalog();
    if (index >= catalog.size() || catalog[index].id != id) {
        return nullptr;
    }
    return &catalog[index];
}

const IconInfo* FindByName(const char* name)
{
    if (name == nullptr || *name == '\0') {
        return nullptr;
    }
    for (const IconInfo& info : Catalog()) {
        if (info.name != nullptr && std::strcmp(info.name, name) == 0) {
            return &info;
        }
    }
    return nullptr;
}

bool IsPetArtwork(IconId id)
{
    return id == IconId::PetArtwork;
}

wxBitmap RenderMonochrome(IconId id, int sizePx, const wxColour& tint)
{
    if (!IsValidId(id) || IsArtwork(id) || FindDef(id) == nullptr) {
        return wxNullBitmap;
    }
    if (sizePx <= 0) {
        return wxNullBitmap;
    }
    const int sizeDip = std::min(sizePx, kMaxRenderDip);
    // DIP → 物理像素：位图按真实像素栅格化，调用方仍按 DIP 尺寸绘制。
    const int pixels = agent::gui::tokens::CssPxToDip(nullptr, sizeDip);
    if (pixels <= 0) {
        return wxNullBitmap;
    }

    // 为什么用 wxImage 上下文而不是 wxMemoryDC：GDI+ 在 wxMemoryDC 的 DIB 上做半透明
    // 合成时会写坏 RGB（实测 7% 不透明黑叠在白底上得到 rgb(57,57,57) alpha=18，
    // 正确结果应是 rgb(239,239,239) alpha=255）。wxGraphicsContext::Create(wxImage&)
    // 建的是真正的 32bppARGB GDI+ Bitmap，src-over 正确；见 icon-map「无法精确表达」。
    wxImage image(pixels, pixels);
    image.InitAlpha();
    std::memset(image.GetAlpha(), 0, static_cast<std::size_t>(pixels) * pixels);

    bool drawn = false;
    {
        // gc 必须比 image 的后续使用先析构（GDI+ 在 gc 析构时把像素写回 wxImage）。
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(image));
        if (gc) {
            drawn = Draw(*gc, id, wxRect(0, 0, pixels, pixels), tint);
        }
    }
    if (!drawn) {
        return wxNullBitmap;
    }
    wxBitmap bitmap(image);
    if (!bitmap.IsOk()) {
        return wxNullBitmap;
    }
    return bitmap;
}


// 说明：Draw/DrawPetArtwork 的 boxPx 是 **gc 当前用户坐标** 里的矩形。窗口 gc 的用户空间
// 已经是 DIP（wx 3.2 在 MSW 上按 DPI 缩放 DC），位图 gc 的用户空间是物理像素；两种情况
// 都不需要再做 DIP→像素换算，否则会二次缩放。唯一需要换算的入口是 RenderMonochrome
// （DIP 尺寸 → 位图像素，那里用 tokens::CssPxToDip）。
bool Draw(wxGraphicsContext& gc, IconId id, const wxRect& boxPx, const wxColour& tint)
{
    if (boxPx.width <= 0 || boxPx.height <= 0 || !IsValidId(id)) {
        return false;
    }
    if (id == IconId::PetArtwork) {
        return DrawPet(gc, boxPx, false);
    }
    if (id == IconId::BrandMark) {
        return DrawBrand(gc, boxPx);
    }
    const IconDef* def = FindDef(id);
    if (def == nullptr) {
        return false;
    }
    return StrokeMonochrome(gc, *def, boxPx, tint);
}

bool DrawPetArtwork(wxGraphicsContext& gc, const wxRect& boxPx, bool narrow)
{
    if (boxPx.width <= 0 || boxPx.height <= 0) {
        return false;
    }
    return DrawPet(gc, boxPx, narrow);
}

}  // namespace agent::gui::icons
