#pragma once
//
// UiLayoutState — 自定义布局状态与持久化（T-250）
//
// 规范依据：docs/GUI-CHUGAO/SigFlow-界面改造SPEC.md v2.0
//   LAY-020 状态模型字段与默认值
//   LAY-025 四个布局预设与「全字段命中才点亮」判定
//   LAY-031 版本化持久化键（wxConfig；禁止第二份 layout.json）
//   LAY-032 旧配置迁移：忽略未知 pane / 坏格式 / 半加载，必要时整份回退默认
//   DEC-01 本轮保留右栏三页；DEC-04 右栏 264（210–520）
//
// 本模块是**纯逻辑 + wxConfig 读写**，不含任何窗口操作，便于单元测试。
// 窗口侧的显隐/对齐/浮动应用由 T-510 的布局引擎消费本状态。
//

#include <wx/string.h>
#include <wx/gdicmn.h>

#include <vector>

class wxConfigBase;

namespace UiTheme {

/// 主侧栏位置（LAY-020）
enum class Side
{
    Left,
    Right
};

/// 底部面板对齐方式（LAY-020 / LAY-029）
enum class Balance
{
    Left,
    Right,
    Center,
    Justify
};

/// 面板位置（LAY-020 / LAY-030）
enum class PanelPos
{
    Bottom,
    Top
};

/// 六栏显隐标识（LAY-020）
enum class Visibility
{
    Menubar,
    Rail,
    Left,
    Right,
    Bottom,
    Statusbar
};

/// 布局状态（LAY-020）
struct LayoutState
{
    // ---- 六栏显隐 ----
    bool menubar   = true;
    bool rail      = true;
    bool left      = true;
    bool right     = true;
    bool bottom    = true;
    bool statusbar = true;

    // ---- 位次与对齐 ----
    Side     side = Side::Left;
    Balance  bal  = Balance::Justify;
    PanelPos pos  = PanelPos::Bottom;

    // ---- 尺寸（DIP）----
    int leftWidth    = 282;  ///< LAY-006：210–460
    int rightWidth   = 264;  ///< DEC-04：210–520
    int bottomHeight = 174;  ///< LAY-009
    int splitPercent = 62;   ///< LAY-010：中区画布占比，25–80

    /// 默认状态（LAY-020：六项全真、side=Left、bal=Justify、pos=Bottom）
    static LayoutState Defaults();

    /// 把尺寸/比例夹取到规范允许的范围（LAY-006 / DEC-04 / LAY-010）
    void ClampToValidRanges();

    bool operator==(const LayoutState& other) const;
    bool operator!=(const LayoutState& other) const { return !(*this == other); }
};

/// 读取/写入某个显隐字段（LAY-020）
bool GetVisibility(const LayoutState& state, Visibility which);
void SetVisibility(LayoutState& state, Visibility which, bool visible);

/// LAY-029: compute the content rectangle within the panel host (DIP limits).
wxRect AlignedPanelRect(const wxSize& host, Balance balance, double dip);

// ---------------------------------------------------------------------------
// 窄屏断点与最小窗口（T-520；LAY-019 / PLT-006 / TEST-020 / A11Y-010）
//
// 设计依据（app.html，唯一权威）：
//   <1024 两 dock 宽度用 clamp（右栏 clamp(108, 14vw, 240)，app.html:1529-1530,744）
//   <760  辅助侧边栏自动隐藏、工具栏横向滚动
//   全程  不得出现横向滚动条、不得控件截断（验收：800×600 / 640×480）
//
// 本段是**纯函数**，不含窗口操作，可直接单元测试（与 LAY-020/025 同一风格）。
// ---------------------------------------------------------------------------

/// 断点档位（LAY-019）
enum class Breakpoint
{
    Wide,    ///< ≥ 1024 DIP：尊重用户配置的 dock 宽度
    Medium,  ///< [760, 1024)：两 dock 宽度用 clamp
    Narrow   ///< < 760：辅助侧边栏自动隐藏
};

inline constexpr int BreakpointNarrow = 760;   ///< LAY-019：<760 隐藏辅助侧栏
inline constexpr int BreakpointMedium = 1024;  ///< LAY-019：<1024 两 dock 用 clamp

/// 按客户区宽度（DIP）判定断点
Breakpoint ForWidth(int clientWidthDip);

/// 两个 dock 的宽度（DIP）
struct DockWidths
{
    int left  = 282;
    int right = 264;
};

/// LAY-019：按断点计算两个 dock 宽度（DIP）。宽屏返回用户配置值（已 clamp 到合法范围）。
DockWidths ClampDockWidths(int clientWidthDip, const LayoutState& state);

/// LAY-019：<760 时辅助侧边栏是否应自动隐藏
inline bool ShouldAutoHideRight(int clientWidthDip)
{
    return clientWidthDip < BreakpointNarrow;
}

/// PLT-006：帧最小尺寸（DIP）。取 640×480 —— 验收正是在该尺寸下检查无横向滚动条。
wxSize MinimumFrameSize();

/// 显隐字段的中文名（用于状态栏反馈，LAY-023；语言遵循 SAFE-014 的决定）
wxString VisibilityLabel(Visibility which);

// ---------------------------------------------------------------------------
// 预设（LAY-025）
// ---------------------------------------------------------------------------
namespace Presets {
inline constexpr const char* Classic = "classic";  ///< 经典：全显
inline constexpr const char* Code    = "code";     ///< 编码优先：隐藏 rail
inline constexpr const char* Canvas  = "canvas";   ///< 画布优先：隐藏 rail + left
inline constexpr const char* Chat    = "chat";     ///< Chat 分离：隐藏 right
}  // namespace Presets

/// 预设 id 列表（顺序固定，用于 UI 渲染）
std::vector<wxString> PresetIds();

/// 预设显示名（与 app.html:804-807 一致，中文）
wxString PresetLabel(const wxString& presetId);

/// 预设显示名（英文；用于状态栏等既有英文文本处，SAFE-014）
wxString PresetLabelEn(const wxString& presetId);

/// 把预设应用到状态（LAY-025）；未知 id 返回 false 且不修改状态
bool ApplyPreset(LayoutState& state, const wxString& presetId);

/// 当前状态是否与某预设**完全一致**（LAY-025：全字段命中才点亮）
bool MatchesPreset(const LayoutState& state, const wxString& presetId);

// ---------------------------------------------------------------------------
// 持久化（LAY-031 / LAY-032）
// ---------------------------------------------------------------------------
class LayoutStore
{
public:
    /// 作用域键前缀：`Ui/Layout/v2/<edition>/<mode>`（LAY-031；与 docs/gui task.md:101 的建议一致）
    static wxString ScopeKey(const wxString& edition, const wxString& mode);

    /// 读取布局；任何字段缺失/损坏都用默认值补齐，并把说明写入 note（LAY-032）
    static LayoutState Load(wxConfigBase* cfg, const wxString& scope, wxString* note = nullptr);

    /// 写入布局；cfg 为空返回 false
    static bool Save(wxConfigBase* cfg, const wxString& scope, const LayoutState& state);

    /// AUI perspective 读写（与 pane 尺寸一同持久化）
    static wxString LoadPerspective(wxConfigBase* cfg, const wxString& scope);
    static bool SavePerspective(wxConfigBase* cfg, const wxString& scope, const wxString& perspective);

    /// perspective 兼容性校验（LAY-032）：必须包含全部 6 个 pane 名称，否则判为不可用
    static bool IsPerspectiveCompatible(const wxString& perspective, wxString* reason = nullptr);

    /// 本版本必须存在的 pane 名称（与 MainFrame 的 AddPane 一致）
    static std::vector<wxString> RequiredPaneNames();

    /// 删除某作用域下的全部布局键（Reset Layout 用；不影响业务状态）
    static bool Clear(wxConfigBase* cfg, const wxString& scope);
};

}  // namespace UiTheme
