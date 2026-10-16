//
// UiLayoutState.cpp — 布局状态、预设与 wxConfig 持久化实现（T-250）
//
// 规范依据：docs/GUI-CHUGAO/SigFlow-界面改造SPEC.md v2.0 §5.2（LAY-020…LAY-032）
// 设计稿溯源：GUI/app.html:1596-1610（layoutState / PRESETS）、:1595（键名 sigflow-layout-v2）
//

#include "ui/UiLayoutState.h"

#include "ui/UiTheme.h"

#include <wx/config.h>

#include <algorithm>
#include <vector>

namespace UiTheme {
wxRect AlignedPanelRect(const wxSize& host, Balance balance, double dip)
{
    const int available = std::max(0, host.x);
    int width = available;
    if (balance == Balance::Left || balance == Balance::Right)
        width = std::min(available, static_cast<int>(760 * dip));
    else if (balance == Balance::Center)
        width = std::min(static_cast<int>(840 * dip), static_cast<int>(available * 0.88));
    const int x = balance == Balance::Right ? available - width
                  : balance == Balance::Center ? (available - width) / 2 : 0;
    return wxRect(x, 0, width, std::max(0, host.y));
}

// ---------------------------------------------------------------------------
// 窄屏断点与最小窗口（T-520；LAY-019 / PLT-006 / TEST-020）
// ---------------------------------------------------------------------------
Breakpoint ForWidth(int clientWidthDip)
{
    if (clientWidthDip < BreakpointNarrow) {
        return Breakpoint::Narrow;
    }
    if (clientWidthDip < BreakpointMedium) {
        return Breakpoint::Medium;
    }
    return Breakpoint::Wide;
}

DockWidths ClampDockWidths(int clientWidthDip, const LayoutState& state)
{
    LayoutState clamped = state;
    clamped.ClampToValidRanges();  // 先保证用户配置本身合法（LAY-006 / DEC-04）

    DockWidths out{ clamped.leftWidth, clamped.rightWidth };
    if (ForWidth(clientWidthDip) == Breakpoint::Wide) {
        return out;  // 宽屏：完全尊重用户配置
    }

    // <1024：两 dock 用 clamp（LAY-019）
    //   左：clamp(210, 22vw, min(282, 用户值))   —— 下界取 LAY-006 的 210
    //   右：clamp(108, 14vw, 240)                —— 与设计稿一致（app.html:1529-1530,744）
    const int leftLower = 210;
    const int leftUpper = std::max(leftLower, std::min(282, clamped.leftWidth));
    out.left = std::clamp(static_cast<int>(clientWidthDip * 0.22), leftLower, leftUpper);

    const int rightLower = 108;
    const int rightUpper = std::max(rightLower, std::min(240, clamped.rightWidth));
    out.right = std::clamp(static_cast<int>(clientWidthDip * 0.14), rightLower, rightUpper);
    return out;
}

wxSize MinimumFrameSize()
{
    // PLT-006 / TEST-020：验收正是在 640×480 下检查"无横向滚动条、无控件截断"，
    // 因此最小尺寸取该值，保证验收尺寸可达且再小也不会被允许。
    return wxSize(640, 480);
}

namespace {

// 布局**结构**版本。改动 pane 分层顺序、新增/删除 pane、或调整默认几何（如 dock 宽度上限）时
// 必须 +1：wxAui 的 perspective 会把分层与**实际像素尺寸**一并持久化，旧存档会把这些新默认值
// 全部覆盖掉（表现为"改了默认值但界面没变"）。换作用域版本 = 旧存档自然失效并回到新默认。
constexpr const char* kScopeRoot = "Ui/Layout/v4";

bool ShapeEquals(const LayoutState& a, const LayoutState& b)
{
    return a.menubar == b.menubar && a.rail == b.rail && a.left == b.left && a.right == b.right &&
           a.bottom == b.bottom && a.statusbar == b.statusbar && a.side == b.side &&
           a.bal == b.bal && a.pos == b.pos;
}

bool IsValidPresetId(const wxString& id)
{
    return id == Presets::Classic || id == Presets::Code || id == Presets::Canvas ||
           id == Presets::Chat;
}

/// 预设只定义「形态」字段（六栏显隐 + 位置/对齐），不含尺寸 —— 与 app.html:1605-1610 一致
LayoutState MakePreset(const wxString& id)
{
    LayoutState s = LayoutState::Defaults();
    if (id == Presets::Code) {
        s.rail = false;
    } else if (id == Presets::Canvas) {
        s.rail = false;
        s.left = false;
    } else if (id == Presets::Chat) {
        s.right = false;
    }
    return s;  // Classic：全部默认（六栏全显）
}

wxString SideToString(Side side) { return side == Side::Right ? wxS("right") : wxS("left"); }

Side SideFromString(const wxString& v, Side fallback)
{
    const wxString s = v.Lower().Trim().Trim(false);
    if (s == wxS("left")) return Side::Left;
    if (s == wxS("right")) return Side::Right;
    return fallback;
}

wxString BalanceToString(Balance bal)
{
    switch (bal) {
        case Balance::Left: return wxS("left");
        case Balance::Right: return wxS("right");
        case Balance::Center: return wxS("center");
        case Balance::Justify: break;
    }
    return wxS("just");
}

Balance BalanceFromString(const wxString& v, Balance fallback)
{
    const wxString s = v.Lower().Trim().Trim(false);
    if (s == wxS("left")) return Balance::Left;
    if (s == wxS("right")) return Balance::Right;
    if (s == wxS("center")) return Balance::Center;
    if (s == wxS("just") || s == wxS("justify")) return Balance::Justify;
    return fallback;
}

wxString PanelPosToString(PanelPos pos) { return pos == PanelPos::Top ? wxS("top") : wxS("bottom"); }

PanelPos PanelPosFromString(const wxString& v, PanelPos fallback)
{
    const wxString s = v.Lower().Trim().Trim(false);
    if (s == wxS("top")) return PanelPos::Top;
    if (s == wxS("bottom")) return PanelPos::Bottom;
    return fallback;
}

/// 读取 long；缺失返回 false（不写 note —— 缺失是正常情况）
bool ReadLong(wxConfigBase* cfg, const wxString& key, long* out)
{
    return cfg->Read(key, out);
}

/// 读取字符串；缺失返回 false
bool ReadString(wxConfigBase* cfg, const wxString& key, wxString* out)
{
    return cfg->Read(key, out);
}

}  // namespace

// ===========================================================================
// LayoutState
// ===========================================================================
LayoutState LayoutState::Defaults()
{
    // LAY-020 默认值 + LAY-006 / DEC-04 / LAY-009 / LAY-010 的尺寸默认值
    return LayoutState{};
}

void LayoutState::ClampToValidRanges()
{
    leftWidth  = std::max(Skeleton::LeftMinW, std::min(Skeleton::LeftMaxW, leftWidth));
    rightWidth = std::max(Skeleton::RightMinW, std::min(Skeleton::RightMaxW, rightWidth));
    bottomHeight = std::max(80, std::min(600, bottomHeight));
    splitPercent = std::max(25, std::min(80, splitPercent));
}

bool LayoutState::operator==(const LayoutState& other) const
{
    return ShapeEquals(*this, other) && leftWidth == other.leftWidth &&
           rightWidth == other.rightWidth && bottomHeight == other.bottomHeight &&
           splitPercent == other.splitPercent;
}

bool GetVisibility(const LayoutState& state, Visibility which)
{
    switch (which) {
        case Visibility::Menubar: return state.menubar;
        case Visibility::Rail: return state.rail;
        case Visibility::Left: return state.left;
        case Visibility::Right: return state.right;
        case Visibility::Bottom: return state.bottom;
        case Visibility::Statusbar: break;
    }
    return state.statusbar;
}

void SetVisibility(LayoutState& state, Visibility which, bool visible)
{
    switch (which) {
        case Visibility::Menubar: state.menubar = visible; return;
        case Visibility::Rail: state.rail = visible; return;
        case Visibility::Left: state.left = visible; return;
        case Visibility::Right: state.right = visible; return;
        case Visibility::Bottom: state.bottom = visible; return;
        case Visibility::Statusbar: break;
    }
    state.statusbar = visible;
}

wxString VisibilityLabel(Visibility which)
{
    switch (which) {
        case Visibility::Menubar: return wxS("Menu Bar");
        case Visibility::Rail: return wxS("Activity Bar");
        case Visibility::Left: return wxS("Primary Side Bar");
        case Visibility::Right: return wxS("Secondary Side Bar");
        case Visibility::Bottom: return wxS("Panel");
        case Visibility::Statusbar: break;
    }
    return wxS("Status Bar");
}

// ===========================================================================
// 预设
// ===========================================================================
std::vector<wxString> PresetIds()
{
    return { Presets::Classic, Presets::Code, Presets::Canvas, Presets::Chat };
}

wxString PresetLabel(const wxString& presetId)
{
    if (presetId == Presets::Classic) return wxS("经典");
    if (presetId == Presets::Code) return wxS("编码优先");
    if (presetId == Presets::Canvas) return wxS("画布优先");
    if (presetId == Presets::Chat) return wxS("Chat 分离");
    return presetId;
}

wxString PresetLabelEn(const wxString& presetId)
{
    if (presetId == Presets::Classic) return wxS("Classic");
    if (presetId == Presets::Code) return wxS("Code First");
    if (presetId == Presets::Canvas) return wxS("Canvas First");
    if (presetId == Presets::Chat) return wxS("Chat Detached");
    return presetId;
}

bool ApplyPreset(LayoutState& state, const wxString& presetId)
{
    if (!IsValidPresetId(presetId)) {
        return false;
    }
    const LayoutState preset = MakePreset(presetId);
    // 只覆盖形态字段，保留用户尺寸
    state.menubar   = preset.menubar;
    state.rail      = preset.rail;
    state.left      = preset.left;
    state.right     = preset.right;
    state.bottom    = preset.bottom;
    state.statusbar = preset.statusbar;
    state.side      = preset.side;
    state.bal       = preset.bal;
    state.pos       = preset.pos;
    return true;
}

bool MatchesPreset(const LayoutState& state, const wxString& presetId)
{
    if (!IsValidPresetId(presetId)) {
        return false;
    }
    return ShapeEquals(state, MakePreset(presetId));
}

// ===========================================================================
// 持久化
// ===========================================================================
wxString LayoutStore::ScopeKey(const wxString& edition, const wxString& mode)
{
    wxString scope = wxString::FromUTF8(kScopeRoot);
    scope << "/" << (edition.empty() ? wxS("pro") : edition)
          << "/" << (mode.empty() ? wxS("workspace") : mode);
    return scope;
}

std::vector<wxString> LayoutStore::RequiredPaneNames()
{
    return { wxS("topBar"), wxS("sideNav"), wxS("left_sidebar"),
             wxS("right_sidebar"), wxS("bottom_tabs"), wxS("center_area") };
}

bool LayoutStore::IsPerspectiveCompatible(const wxString& perspective, wxString* reason)
{
    if (perspective.empty()) {
        if (reason) *reason = wxS("perspective 为空");
        return false;
    }
    for (const wxString& name : RequiredPaneNames()) {
        if (perspective.Find(name) == wxNOT_FOUND) {
            if (reason) *reason = wxS("perspective 缺少 pane：") + name;
            return false;
        }
    }
    return true;
}

LayoutState LayoutStore::Load(wxConfigBase* cfg, const wxString& scope, wxString* note)
{
    LayoutState state = LayoutState::Defaults();
    if (cfg == nullptr) {
        if (note) *note = wxS("配置后端不可用，使用默认布局");
        return state;
    }

    std::vector<wxString> problems;

    // ---- 六栏显隐 ----
    struct BoolField
    {
        const char* name;
        bool LayoutState::*member;
    };
    const BoolField boolFields[] = {
        { "Menubar", &LayoutState::menubar }, { "Rail", &LayoutState::rail },
        { "Left", &LayoutState::left },       { "Right", &LayoutState::right },
        { "Bottom", &LayoutState::bottom },   { "StatusBar", &LayoutState::statusbar },
    };
    for (const BoolField& f : boolFields) {
        long value = 0;
        if (ReadLong(cfg, scope + "/" + f.name, &value)) {
            state.*(f.member) = (value != 0);
        }
    }

    // ---- 位次与对齐（字符串；非法值回退默认并记录）----
    wxString text;
    if (ReadString(cfg, scope + "/Side", &text)) {
        const Side parsed = SideFromString(text, state.side);
        if (parsed == state.side && text.Lower().Trim().Trim(false) != SideToString(state.side)) {
            problems.push_back(wxS("Side=") + text);
        }
        state.side = parsed;
    }
    if (ReadString(cfg, scope + "/Balance", &text)) {
        const Balance parsed = BalanceFromString(text, state.bal);
        if (parsed == state.bal && text.Lower().Trim().Trim(false) != BalanceToString(state.bal)) {
            problems.push_back(wxS("Balance=") + text);
        }
        state.bal = parsed;
    }
    if (ReadString(cfg, scope + "/PanelPos", &text)) {
        const PanelPos parsed = PanelPosFromString(text, state.pos);
        if (parsed == state.pos && text.Lower().Trim().Trim(false) != PanelPosToString(state.pos)) {
            problems.push_back(wxS("PanelPos=") + text);
        }
        state.pos = parsed;
    }

    // ---- 尺寸与比例（超出范围即夹取并记录）----
    struct IntField
    {
        const char* name;
        int LayoutState::*member;
        int low;
        int high;
    };
    const IntField intFields[] = {
        { "LeftWidth", &LayoutState::leftWidth, Skeleton::LeftMinW, Skeleton::LeftMaxW },
        { "RightWidth", &LayoutState::rightWidth, Skeleton::RightMinW, Skeleton::RightMaxW },
        { "BottomHeight", &LayoutState::bottomHeight, 80, 600 },
        { "SplitPercent", &LayoutState::splitPercent, 25, 80 },
    };
    for (const IntField& f : intFields) {
        long value = 0;
        if (!ReadLong(cfg, scope + "/" + f.name, &value)) {
            continue;
        }
        int parsed = static_cast<int>(value);
        if (parsed < f.low || parsed > f.high) {
            // 注意：wxString::Format 的宽字符格式串中 %s 需要 wchar_t*，
            // f.name 是 const char*，必须先转换，否则是未定义行为（同类崩溃隐患）。
            problems.push_back(wxString::Format(wxS("%s=%d 越界[%d,%d]"),
                                                wxString::FromUTF8(f.name), parsed, f.low, f.high));
            parsed = std::max(f.low, std::min(f.high, parsed));
        }
        state.*(f.member) = parsed;
    }

    state.ClampToValidRanges();  // 兜底

    if (note != nullptr) {
        if (problems.empty()) {
            note->clear();
        } else {
            wxString joined;
            for (size_t i = 0; i < problems.size(); ++i) {
                if (i > 0) {
                    joined << wxS("; ");
                }
                joined << problems[i];
            }
            *note = wxS("布局配置存在无效项，已按默认/边界修正：") + joined;
        }
    }
    return state;
}

bool LayoutStore::Save(wxConfigBase* cfg, const wxString& scope, const LayoutState& state)
{
    if (cfg == nullptr) {
        return false;
    }
    LayoutState clamped = state;
    clamped.ClampToValidRanges();

    cfg->Write(scope + "/Menubar", clamped.menubar);
    cfg->Write(scope + "/Rail", clamped.rail);
    cfg->Write(scope + "/Left", clamped.left);
    cfg->Write(scope + "/Right", clamped.right);
    cfg->Write(scope + "/Bottom", clamped.bottom);
    cfg->Write(scope + "/StatusBar", clamped.statusbar);
    cfg->Write(scope + "/Side", SideToString(clamped.side));
    cfg->Write(scope + "/Balance", BalanceToString(clamped.bal));
    cfg->Write(scope + "/PanelPos", PanelPosToString(clamped.pos));
    cfg->Write(scope + "/LeftWidth", static_cast<long>(clamped.leftWidth));
    cfg->Write(scope + "/RightWidth", static_cast<long>(clamped.rightWidth));
    cfg->Write(scope + "/BottomHeight", static_cast<long>(clamped.bottomHeight));
    cfg->Write(scope + "/SplitPercent", static_cast<long>(clamped.splitPercent));
    return cfg->Flush();
}

wxString LayoutStore::LoadPerspective(wxConfigBase* cfg, const wxString& scope)
{
    if (cfg == nullptr) {
        return wxString();
    }
    wxString perspective;
    if (!cfg->Read(scope + "/Perspective", &perspective)) {
        return wxString();
    }
    wxString reason;
    if (!IsPerspectiveCompatible(perspective, &reason)) {
        // LAY-032：不可安全迁移则整份忽略，回退默认布局
        return wxString();
    }
    return perspective;
}

bool LayoutStore::SavePerspective(wxConfigBase* cfg, const wxString& scope, const wxString& perspective)
{
    if (cfg == nullptr || perspective.empty()) {
        return false;
    }
    cfg->Write(scope + "/Perspective", perspective);
    return cfg->Flush();
}

bool LayoutStore::Clear(wxConfigBase* cfg, const wxString& scope)
{
    if (cfg == nullptr) {
        return false;
    }
    // 只删布局子树，不触碰 MRU / 主题 / 迁移标记（SAFE-005 / LAY-032）
    const bool deleted = cfg->DeleteGroup(scope);
    cfg->Flush();
    return deleted;
}

}  // namespace UiTheme
