#pragma once
//
// AgentControls — AGUI-T05 可复用原生控件与圆角内容宿主
//
// 覆盖的审查条目（design-review.md）：
//   VIS-002 玻璃白 .70/.84 与高光层次 → 预合成实色（SAFE-008：wx 侧不做真实模糊）
//   VIS-004 1px 边线 / 10 / 6 / pill 圆角 → AgentMetrics::CardRadius / ControlRadius /
//           PillRadius（经 AgentTokens::Tokens(Component).radiusDip 取出）
//   VIS-007 按钮 normal/hover/pressed/selected/disabled/focus → 本文件的状态机
//   LAY-009 圆角内无旧直角边框、内容不被装饰遮挡 → AgentCardHost / AgentAccordion
//           把真实子 HWND 内缩到圆角切点内（**不**用 SetWindowRgn 裁剪子窗口）
//   CMP-006 六个独立 details、默认展开、允许多开 → AgentAccordion（各节互不影响）
//   INT-014 折叠 / 滚动 / 焦点可达 → 内容仍是调用方的原生控件，标题行可键盘操作
//
// 铁律（DEC-13 / SAFE-012 / THE-001）：
//   * 颜色、字号、行高、圆角、内距**只**取 AgentTokens：
//     Tokens(Component) / SurfaceColour / TextColour / BorderColour(Component) /
//     BorderColour(BorderRole) / CssPxToDip / UiFontPx / LineHeightDip / Colours()。
//     本文件（含 .cpp）没有 wxColour(r,g,b)、没有 hex 字面量、没有第二套调色板。
//     AgentTokens 尚未暴露的**结构尺寸**（FocusGlow、FocusRingOffset、Accordion 内距、
//     分段内距、KvKeyWidth）直接引用 UiTheme 的 Agent 命名空间（仍是单一真源），
//     已作为「待补门面」项报告给协调者，而不是在控件层另建常量。
//   * 绘制与输入分离：每次 wxEVT_PAINT 都是 wxAutoBufferedPaintDC + wxGraphicsContext；
//     事件处理只改状态并 Refresh，不在事件里直接画。
//   * 焦点按**原型**：H85 `:focus-visible{outline:2px solid var(--accent);offset:2px}`
//     + H257 `0 0 0 3px accent-soft` 光晕。与旧工具栏「3 DIP 蓝环 / 仅 hover 蓝边」
//     规则不同，因此本文件自绘；只用 UiFocusRing::Tracker（UiFocusTrace）区分
//     「键盘来源」与「点击后保留焦点」，不套用旧控件规则。
//   * 圆角宿主不裁剪子 HWND：内容窗格按 r*(1-1/√2) 与 AgentMetrics::CardInset 取大者
//     内缩，保证原生子控件（wxTextCtrl / wxListBox / wxScrolledWindow）的方角不压到弧线。
//   * 图标由 AGUI-T06 的 agent::gui::icons 负责；本文件不依赖它，只在需要图标的位置
//     预留槽位矩形（Accordion 的 caret、Pill 的语义圆点），未绘制矢量图标。
//   * 可访问性：每个可交互控件 SetName()，禁用原因写入 DisabledReason() 并同步到
//     tooltip 与 wxAccessible 的帮助/描述文本；可交互控件挂 wxAccessible（角色、
//     名称、可用/选中/焦点状态）。
//     ⚠️ 仅「设置并可通过 API 读回」——本轮**没有**在 Windows 读屏（Narrator / NVDA）
//        实机验证，也没有验证 IME 与真实鼠标轨迹；不得据此宣称读屏通过。
//   * 主题：每个控件持有 UiTheme::Subscription 成员（经 tokens::SubscribeTheme），
//     析构自动解绑。成员先于 wxWindow 基类析构，因此不存在「窗口已拆、回调仍在」。
//     测试用 UiTheme::RefreshFromSystem()（只通知，不写用户配置）验证释放。
//
// 事件回调口径（重要，含一处与需求文字的偏差）：
//   * AgentButton 激活发 wxEVT_BUTTON（id = 本控件 id），每次激活恰好一次。
//   * AgentSegmented 选择变化发 EVT_AGENT_SEGMENT，GetSelection() = segmentId。
//   * AgentAccordion 展开/收起发 EVT_AGENT_ACCORDION：
//       GetSelection() == GetInt() == **section index**
//       GetExtraLong() == 1 展开 / 0 收起；GetString() == "expanded" / "collapsed"
//     ⚠️ wxCommandEvent 的 GetSelection()、GetInt()、IsChecked() 读的是**同一个字段**
//     m_commandInt（wx/event.h:1269 / 1645 / 1648），无法同时表达「index」与「1/0」。
//     这里按第一条（index）落实，展开标志放到 GetExtraLong()/GetString()；
//     如需字面 Int()==1/0，请协调者决定「Int 放标志、index 另寻通道」或加事件子类。
//   * 程序化调用 SetSelection / SetExpanded **不发**业务事件（只有鼠标/键盘触发的
//     用户操作才发）；这与 wx 其它控件的 SetValue 语义一致。
//
// 诊断用 API（不是样式 API）：AgentButton::GetState()/IsFocusVisible()、
//   AgentSegmented::SegmentWindow()、AgentAccordion::SectionHeaderWindow()/
//   HeaderRectPx() —— 供 gallery 定位与 smoke 测试模拟输入，不参与布局决策。
//

#include "agent/gui/AgentIcons.h"
#include "agent/gui/AgentTokens.h"
#include "ui/UiFocusRing.h"
#include "ui/UiTheme.h"

#include <wx/control.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/string.h>

#include <vector>

namespace agent::gui::controls {

/// 按钮变体（原型 .rb / .act / .act.ghost；H179-182、H309-320）
enum class Variant
{
    Primary,   ///< 主行动：H74 135° 蓝色渐变（ctaTop→ctaBottom）+ 白字（.rb.primary / .send）
    Secondary, ///< 常规：H250 面板白底 + hairline + text2，hover 走 accent-soft
    Ghost      ///< 幽灵：H182 .act.ghost 无边框，hover 才出现浅底
};

/// 胶囊语义（原型 .pill.red/.blue/.green/.amber + 中性，H159-164）
enum class Tone
{
    Neutral,
    Accent,
    Success,
    Warn,
    Danger
};

/// 分段选择变化（GetSelection() = segmentId）
wxDECLARE_EVENT(EVT_AGENT_SEGMENT, wxCommandEvent);
/// 折叠展开/收起（GetSelection() = section index；GetExtraLong() = 1 展开 / 0 收起）
wxDECLARE_EVENT(EVT_AGENT_ACCORDION, wxCommandEvent);

class SegmentCell;   ///< 内部实现：单个分段单元（定义在 .cpp）
class SectionHeader; ///< 内部实现：单条折叠标题行（定义在 .cpp）

// ---------------------------------------------------------------------------
// AgentButton — 自绘按钮（VIS-007 / A11Y-001）
//
// 状态优先级（GetState() 的取值顺序，高者胜）：
//   Disabled > Pressed > Selected > Hover > Normal
// FocusVisible 与上面并列独立：只有「键盘来源的焦点」才画 2 DIP accent 环
// + 3 DIP accent-soft 光晕（IsFocusVisible()）。
// ---------------------------------------------------------------------------
class AgentButton : public wxControl
{
public:
    /// 交互态（可观测状态机；FocusVisible 用 IsFocusVisible() 单独查询）
    enum class State
    {
        Normal,
        Hover,
        Pressed,
        Selected,
        Disabled
    };

    AgentButton(wxWindow* parent, wxWindowID id, const wxString& label,
                Variant variant = Variant::Secondary);
    ~AgentButton() override;

    void SetVariant(Variant variant);
    Variant GetVariant() const { return m_variant; }

    void SetLabel(const wxString& label) override;
    wxString GetLabel() const override { return m_label; }

    /// AGUI-T13：图标按钮（原型 `.send` 40×40 白箭头）。设置后只画图标，不画文字；
    /// 激活/焦点/禁用/可访问语义与文字按钮完全一致，可访问名称仍由 SetAccessibleName 提供。
    void SetIcon(icons::IconId icon, int sizeDip);
    bool HasIcon() const { return m_hasIcon; }

    void SetSelected(bool selected);
    bool IsSelected() const { return m_selected; }

    /// 非空 reason 会 Enable(false)，并同步 tooltip 与 wxAccessible 帮助文本；
    /// 清空 reason **不会**自动启用（避免误启用），需要时显式 Enable(true)。
    void SetDisabledReason(const wxString& reason);
    wxString DisabledReason() const { return m_disabledReason; }

    /// 可访问名称：同时 SetName()（wx 控件名）与 wxAccessible 名称来源。
    void SetAccessibleName(const wxString& name);
    wxString AccessibleName() const { return m_accessibleName; }

    State GetState() const;
    /// 键盘来源焦点且本控件持有焦点时才为 true（画环的判据）
    bool IsFocusVisible() const;

    /// 禁用/启用：会同步可访问状态，并在禁用时释放鼠标捕获与按压态。
    bool Enable(bool enable = true) override;

    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override { return AcceptsFocus(); }

    /// 诊断用：激活计数（每次 Activate 自增；测试用它断言「恰好一次」不依赖绑定点）
    long ActivationCount() const { return m_activationCount; }

protected:
    wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent& event);
    void OnLeftDown(wxMouseEvent& event);
    void OnLeftUp(wxMouseEvent& event);
    void OnCaptureLost(wxMouseCaptureLostEvent& event);
    void OnEnter(wxMouseEvent& event);
    void OnLeave(wxMouseEvent& event);
    void OnKeyDown(wxKeyEvent& event);
    void OnKeyUp(wxKeyEvent& event);
    void OnFocus(wxFocusEvent& event);
    void OnKillFocus(wxFocusEvent& event);

    void Activate();
    void ApplyTheme();
    void UpdateVisualState();
    void ResetPressState();
    wxColour CornerSurface() const;  ///< 圆角外角填充（近似父层，避免方角）
    wxColour StateSurface() const;   ///< 圆角内主体填充
    wxColour StateText() const;
    bool StateHasBorder() const;
    void ApplyAccessibility();

    Variant m_variant = Variant::Secondary;
    wxString m_label;
    wxString m_disabledReason;
    wxString m_accessibleName;
    icons::IconId m_icon = icons::IconId::Unknown;
    int m_iconDip = 0;
    bool m_hasIcon = false;
    bool m_selected = false;
    bool m_hover = false;
    bool m_mousePressed = false;
    bool m_keyPressed = false;
    bool m_spaceDown = false;
    bool m_enterDown = false;
    long m_activationCount = 0;
    UiFocusRing::Tracker m_focusTracker;
    UiTheme::Subscription m_theme;
};

// ---------------------------------------------------------------------------
// AgentSegmented — 分段选择（H96-98 .mode-seg / H250-252 .level / H324 .lv-seg）
//
// 实现：容器只画 H96 的 sunken 底 + pill 边框；每个分段是真实的 wxControl 子窗口
// （可 SetName，参与 roving tabindex：只有选中项在 Tab 链里，左右键移动焦点并选中）。
// ---------------------------------------------------------------------------
class AgentSegmented : public wxPanel
{
public:
    AgentSegmented(wxWindow* parent, wxWindowID id);
    ~AgentSegmented() override;

    /// 返回新分段的下标（从 0 起）；segmentId 重复时返回既有下标（不新建）。
    int AddSegment(int segmentId, const wxString& label, const wxString& accessibleName = {});
    int SegmentCount() const { return static_cast<int>(m_entries.size()); }

    /// 程序化设置选中（不发事件）；segmentId 不存在时保持原值。
    void SetSelection(int segmentId);
    /// 当前选中 segmentId；无选中返回 -1。
    int GetSelection() const { return m_selection; }

    void SetSegmentEnabled(int segmentId, bool enabled, const wxString& reason = {});
    bool IsSegmentEnabled(int segmentId) const;

    /// 诊断用：分段单元窗口（模拟输入/挂 tooltip）
    wxWindow* SegmentWindow(int segmentId) const;

    bool AcceptsFocus() const override { return false; }

protected:
    wxSize DoGetBestSize() const override;

private:
    friend class SegmentCell;

    struct Entry
    {
        int id = -1;
        wxString label;
        wxString name;
        bool enabled = true;
        wxString reason;
    };

    int IndexOf(int segmentId) const;
    int FirstEnabled() const;
    bool IsTabStopCell(int index) const;
    void SelectByUser(int index);
    void StepSelection(int fromIndex, int delta);
    void ApplyEntry(int index);
    void RefreshCells();
    void ApplyTheme();
    void OnPaint(wxPaintEvent& event);
    void Arrange();
    int PadPx() const;
    int GapPx() const;

    std::vector<Entry> m_entries;
    std::vector<SegmentCell*> m_cells;
    int m_selection = -1;
    UiTheme::Subscription m_theme;
};

// ---------------------------------------------------------------------------
// AgentPill — 状态胶囊（H159-164）
// ---------------------------------------------------------------------------
class AgentPill : public wxPanel
{
public:
    AgentPill(wxWindow* parent, wxWindowID id, const wxString& text,
              Tone tone = Tone::Neutral);
    ~AgentPill() override;

    void SetTone(Tone tone);
    Tone GetTone() const { return m_tone; }
    void SetText(const wxString& text);
    wxString GetText() const { return m_text; }

    bool AcceptsFocus() const override { return false; }

protected:
    wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent& event);
    void ApplyTheme();

    wxString m_text;
    Tone m_tone = Tone::Neutral;
    UiTheme::Subscription m_theme;
};

// ---------------------------------------------------------------------------
// AgentKvRow — 键值行（H154-156 .kv / H246-248 .card-b .line）
// ---------------------------------------------------------------------------
class AgentKvRow : public wxPanel
{
public:
    AgentKvRow(wxWindow* parent, const wxString& key, const wxString& value);
    ~AgentKvRow() override;

    void SetKey(const wxString& key);
    wxString GetKey() const { return m_key; }
    void SetValue(const wxString& value);
    wxString GetValue() const { return m_value; }

    /// 值列可用宽度（像素）：内容按此裁剪并加省略号，超出不越界
    int ValueWidthPx() const;

    bool AcceptsFocus() const override { return false; }

protected:
    wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent& event);
    void ApplyTheme();

    wxString m_key;
    wxString m_value;
    UiTheme::Subscription m_theme;
};

// ---------------------------------------------------------------------------
// AgentCardHost — 圆角玻璃卡片 + 内容宿主（H242-245 .card；LAY-009）
//
// 内容窗格按 max(AgentMetrics::CardInset, ceil(r*(1-1/√2))) 内缩；圆角只画在本宿主，
// 子 HWND **不**被 SetWindowRgn 裁剪，也不会盖住弧线。内容所有权：SetContent 把内容
// Reparent 到本宿主（成为子窗口，随宿主销毁）。再次 SetContent 时旧内容只被隐藏并
// 留在宿主内（不销毁、不脱离父窗口），避免出现游离顶层窗口。
// ---------------------------------------------------------------------------
class AgentCardHost : public wxPanel
{
public:
    AgentCardHost(wxWindow* parent, wxWindowID id, const wxString& title = {});
    ~AgentCardHost() override;

    void SetContent(wxWindow* content);
    wxWindow* Content() const { return m_content; }

    /// 内容区（客户区坐标，已内缩，宽高 >= 0，且不越出客户区）
    wxRect ContentRectPx() const;
    /// 标题栏高度（无标题时为 0）
    int HeaderHeightPx() const;

    void SetTitle(const wxString& title);
    wxString GetTitle() const { return m_title; }

    bool AcceptsFocus() const override { return false; }

protected:
    wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent& event);
    void ApplyTheme();
    void Arrange();
    int InsetPx() const;
    int RadiusPx() const;

    wxString m_title;
    wxWindow* m_content = nullptr;
    UiTheme::Subscription m_theme;
};

// ---------------------------------------------------------------------------
// AgentAccordion — 可多开的折叠列表（H143-153 .acc；CMP-006 / INT-014）
//
// 每一节：可聚焦标题行（Space/Enter 切换，上下键移动焦点）+ 真实内容窗口。
// 各节展开状态独立（允许同时多开）；内容窗口 Reparent 到本控件（随宿主销毁）。
// ⚠️ AddSection 之后 BestSize 会失效但不会主动重排父窗口；使用 sizer 的宿主请在
//    添加完分节后自行调用 GetParent()->Layout()。SetExpanded/Toggle 会主动触发一次
//    父窗口 Layout（因为展开/收起改变了自身高度）。
// ---------------------------------------------------------------------------
class AgentAccordion : public wxPanel
{
public:
    AgentAccordion(wxWindow* parent, wxWindowID id);
    ~AgentAccordion() override;

    /// 返回新节下标（从 0 起）。content 可为 nullptr（占位标题，无正文）。
    int AddSection(const wxString& title, wxWindow* content, bool expanded = false);
    void SetSectionTitle(int index, const wxString& title);

    /// 程序化展开/收起（不发事件）
    void SetExpanded(int index, bool expanded);
    bool IsExpanded(int index) const;
    int SectionCount() const { return static_cast<int>(m_sections.size()); }

    wxWindow* SectionContent(int index) const;
    /// 诊断用：标题行窗口（模拟输入 / 挂 tooltip）
    wxWindow* SectionHeaderWindow(int index) const;
    /// 诊断/布局用：标题行矩形（客户区坐标；无效下标返回空矩形）
    wxRect HeaderRectPx(int index) const;
    /// 诊断/布局用：正文矩形（客户区坐标；收起或无内容时为空矩形）
    wxRect BodyRectPx(int index) const;

    /// AGUI-T09 追加：宿主宽度变化后按当前宽度重排分节。
    /// AddSection/SetExpanded 之外，调用方改了正文控件的最小尺寸/文本后需要主动调用；
    /// 不发业务事件，只重算几何并重绘。
    void Relayout();

    bool AcceptsFocus() const override { return false; }

protected:
    wxSize DoGetBestSize() const override;

private:
    friend class SectionHeader;

    struct Section
    {
        wxString title;
        wxWindow* content = nullptr;
        bool expanded = false;
    };

    int HeaderHeightPx() const;
    int BodyHeightPx(int index) const;
    void ToggleByUser(int index);
    void MoveHeaderFocus(int index, int delta);
    void ApplySection(int index);
    void RefreshHeaders();
    void ApplyTheme();
    void OnPaint(wxPaintEvent& event);
    void Arrange();

    std::vector<Section> m_sections;
    std::vector<SectionHeader*> m_headers;
    UiTheme::Subscription m_theme;
};

}  // namespace agent::gui::controls
