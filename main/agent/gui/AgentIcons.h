//
// AgentIcons.h — AGUI-T06 图标与双球矢量资源（wxGraphicsContext 原生绘制）
//
// 设计源：GUI/agent-gui/index.html（内联 SVG）、spec.md §4.2/§4.7/§2.3/§10、
//         design-review.md §3/C01/C14、phase1/token-map.md §9/§13。
// 逐图标溯源（稳定 ID / 中文用途 / 行号 / 原 viewBox / 目标 DIP / 着色方式）：
//         GUI/agent-gui/phase1/icon-map.md
//
// 为什么是原生矢量绘制：
//   * 工程内没有 wxSVG；3rd/nanosvg/nanosvg.h 存在但**未被构建使用**（无 nanosvgrast、
//     无 CMake 接线）。T06 不新增依赖，也不接 wxWebView、不读外部 PNG/SVG 文件。
//   * 所有单色图标与双球 artwork 都在 wxGraphicsContext 里用路径/画刷画出来，
//     因此不依赖 cwd、不落盘、不需要资源搜索路径。
//
// 关键口径（调用方必须遵守）：
//   1. 尺寸单位是 **DIP**：`defaultSizeDip`、`RenderMonochrome(sizePx)` 与
//      `Draw*(boxPx)` 的边长都按 DIP 理解（与 AgentTokens::CssPxToDip 同一口径）。
//   2. `RenderMonochrome` 返回的位图**像素尺寸**是 `CssPxToDip(sizePx)`（HiDPI 下更大），
//      调用方按 `sizePx` DIP 绘制即可得到清晰结果（100% DPI 时两者相等）。
//      `Draw` / `DrawPetArtwork` 不额外做 DPI 换算：它们直接在当前 gc 的用户坐标空间里
//      绘制 boxPx，窗口 gc（DIP 空间）与位图 gc（物理像素空间）都正确。
//   3. 颜色：
//      * 单色图标：透明背景，全部描边用调用方传入的 `tint`
//        （调用方从 AgentTokens::TextColour / Colours() 取，本文件不写任何色值字面量）。
//      * 双球 artwork：颜色**只**来自 UiTheme::AgentColours 的 pet* 固定 artwork 组
//        （浅/深两档逐字节同值，见 token-map §13-14），本文件不出现 hex/`wxColour(r,g,b)`。
//   4. 装饰性：`decorative == true` 的图标（rail 品牌缩略、桌宠角标气泡）**不得**被当作
//      按钮的唯一标签；窗口灯 / 对话窗三圆点 / CSS 圆点属于纯装饰，**不进入** IconId 枚举
//      （见 icon-map 排除表）。
//
#pragma once

#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/gdicmn.h>

#include <vector>

class wxGraphicsContext;

namespace agent::gui::icons {

/// 稳定 IconId（顺序即稳定契约：只能追加，不得插入/重排）。
/// 名称里 `Rail*` / `Form*` 对应 index.html 的 `.rb` / `.wt` 两组同义图标。
enum class IconId : int
{
    Unknown = 0,      ///< 无效图标（Find 返回 nullptr；任何绘制请求都失败）

    // ---- rail 功能入口（H399–H423，`.rb svg` 18×18，stroke 1.9）----
    NewAsk,           ///< 新建提问（primary 十字）
    RailGuide,        ///< 学习路径 · 六步（三点折线）
    RailIssue,        ///< 当前问题（靶心）
    RailEvidence,     ///< 证据链（漏斗/滤器）
    RailPlan,         ///< 验证计划（剪贴板 + 勾）
    RailCourse,       ///< 课程引用（书本）
    RailRecord,       ///< 学习记录（书签）
    RailChecker,      ///< 检查器与状态（盾牌 + 勾）

    // ---- 形态切换（rail H425–H436 与对话头 H625–H636 为同一几何，两种尺寸）----
    FormCenter,       ///< 主对话（气泡）
    FormDock,         ///< 右侧停靠（面板分栏）
    FormFloat,        ///< 悬浮窗（窗口 + 标题栏）
    FormPet,          ///< 桌宠待命（圆角脸 + 眼 + 天线）

    // ---- 输入 / 折叠 / 勾选 ----
    ComposerSend,     ///< 发送箭头（H651 16px；桌宠 H502 13px）
    CaretRight,       ///< 折叠箭头（H521 等 12px，展开时 rotate 90°）
    CheckMark,        ///< 勾（H701 8px / H845 9px / H910 11px）

    // ---- 装饰性单色（可 tint，但不得作为按钮唯一标签）----
    PetBadgeChat,     ///< 桌宠右上角对话气泡（aria-hidden）

    // ---- artwork（多色、固定画刷；不走单色 tint 路径）----
    BrandMark,        ///< rail 品牌缩略（H397，viewBox 170，36 DIP，**与桌宠几何不同**）
    PetArtwork,       ///< 桌宠双球（H463–H494，viewBox 170，170 / 140 两档）

    Count             ///< 哨兵：Catalog().size() 应等于它
};

/// 图单元数据（字段顺序是契约，只追加不重排）。
struct IconInfo
{
    IconId id;              ///< 稳定 ID
    const char* name;       ///< 稳定英文 ID（kebab-case，大小写敏感；中文用途见 icon-map.md）
    const char* sourceRef;  ///< 设计源定位，如 "index.html H400 (.rb#newAsk svg)"；非空
    bool decorative;        ///< true = 纯装饰（不得作为按钮唯一标签）
    bool functional;        ///< true = 承担功能语义（与 decorative 互斥）
    int defaultSizeDip;     ///< 原型里的目标边长（DIP）；功能图标恒 > 0
};

/// 全部图标（顺序 == IconId 顺序，`Catalog().size() == Count - 1`）。
/// 头文件契约：`Catalog()[i].id == static_cast<IconId>(i + 1)`。
const std::vector<IconInfo>& Catalog();

/// 按 ID 查（Unknown / 越界 → nullptr）。返回指向 Catalog() 内部的稳定指针。
const IconInfo* Find(IconId id);

/// 按 `IconInfo::name` 精确匹配（大小写敏感；未知 / nullptr → nullptr）。
const IconInfo* FindByName(const char* name);

/// 是否桌宠双球 artwork（只有 PetArtwork 为 true；BrandMark 不是）。
bool IsPetArtwork(IconId id);

/// 单色图标：透明背景 + 只用 tint 描边。
///   * `sizePx` 为 DIP；返回位图像素尺寸 = CssPxToDip(sizePx)；
///   * sizePx <= 0 或 > 512（夹紧上限）按 512 处理；<= 0 返回 wxNullBitmap；
///   * Unknown / 非单色 artwork（BrandMark、PetArtwork）→ wxNullBitmap。
wxBitmap RenderMonochrome(IconId id, int sizePx, const wxColour& tint);

/// 通用绘制（在 gc 的 boxPx 内绘制，boxPx 用 gc 的用户坐标；宽高不等时按短边居中，
/// **不拉伸**）。单色图标用 tint；BrandMark 用固定 artwork 画刷；PetArtwork 等价于
/// `DrawPetArtwork(gc, boxPx, false)`。未知 ID / 空 box → false（不绘制）。
bool Draw(wxGraphicsContext& gc, IconId id, const wxRect& boxPx, const wxColour& tint);

/// 双球 artwork（固定 artwork 色，boxPx 为目标方形）。
///   * 几何：viewBox 170；蓝球 (44,66) r34、红球 (124,106) r38、绿带 #26813D 宽 8 在球后、
///     白高光与眼睛、地面椭圆（H464–H493）；
///   * `narrow == false`（170 大图档）：按 box 短边绘制；
///   * `narrow == true`（140 小图档，H355）：几何逐点相同，但目标边长封顶 140 DIP
///     （box 更大时按 140 居中绘制），用于 ≤560 断点的小图；
///   * 颜色只取 UiTheme::AgentColours 的 pet* 固定 artwork 组；
///   * 空 box（任一边 <= 0）→ false。
bool DrawPetArtwork(wxGraphicsContext& gc, const wxRect& boxPx, bool narrow);

}  // namespace agent::gui::icons
