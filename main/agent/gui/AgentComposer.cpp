// AgentComposer — 统一提问输入区的原生实现（设计源：spec.md §4.6、index.html H254–261、
// H650–651；实现口径见 GUI/agent-gui/implementation.md 中「统一提问 Composer 与草稿」一节）
//
// 组成与画法（为什么这样画）：
//   * 输入壳（40 DIP 行、r-sm、Composer surface）由本面板绘制，原生 wxTextCtrl 只占
//     **内缩后**的区域。原因：子 HWND 永远画在父窗口之上，父窗口画的圆角与 1 DIP 边线
//     若落在子窗口矩形内就必然被遮住（与 AgentControls 的圆角宿主同一结论，故不裁剪子窗口，
//     而是让子窗口避开装饰）。
//   * 焦点态 = 1 DIP accent 边 + 3 DIP accent-soft 光晕。光晕按 CSS `box-shadow:
//     0 0 0 3px` 的落点画在输入壳**外侧**（面板的 11/14/13 内距里），因此行高仍是 40 DIP，
//     焦点切换不会改变布局。
//   * 发送钮是 controls::AgentButton（Primary 渐变 + 16 DIP 图标，40×40）：禁用外观、
//     键盘可达与可访问状态都由它负责；本面板只决定「能不能发」，并把原因交给
//     SetDisabledReason（busy 优先于门禁原因）。
//   * 反馈行是 wxStaticText（wxST_NO_AUTORESIZE）：空即隐藏；文案按真实字体度量折行
//     （tokens::WrapTextForWidth），高度取测量值，不猜行数、不截断。
//
// 边界：颜色与尺寸只经 agent::gui::tokens 取（无颜色字面量、无第二套调色板）；无定时器、
// 无网络、无配置写入、无剪贴板、无 HTML。空白/门禁/提交次数全部走 AgentSubmit 的纯逻辑，
// 长度上限属宿主协议（本文件不另立上限）。IME 状态按真实 Win32 查询，且可被测试覆盖。
#include "agent/gui/AgentComposer.h"

#include "agent/gui/AgentControls.h"
#include "agent/gui/AgentIcons.h"
#include "agent/gui/AgentTokens.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace agent::gui {
namespace {

namespace S = UiTheme::AgentSpacing;
namespace M = UiTheme::AgentMetrics;
namespace T = UiTheme::AgentTypography;

wxString Text(const char* utf8)
{
    return wxString::FromUTF8(utf8);
}

wxString Text(const std::string& utf8)
{
    return wxString::FromUTF8(utf8.data(), utf8.size());
}

int Dip(const wxWindow* window, int value)
{
    return tokens::CssPxToDip(window, value);
}

/// 组件档：颜色角色 / 圆角 / 边线 / 图标 / 内距的唯一来源。
const tokens::ComponentTokens& ComposerTokens()
{
    return tokens::Tokens(tokens::Component::Composer);
}

/// 输入框占位文案（spec §4.6 原文，产品文案里不带任务编号）。
const char* PlaceholderCopy()
{
    return "问点什么，例如：为什么 sum 会保持旧值？";
}

wxString BusyText()
{
    return Text(submit::RejectText(submit::Reject::Busy));
}

wxString EmptyText()
{
    return Text(submit::RejectText(submit::Reject::Empty));
}

/// 门禁未满足时的本地说明：优先用宿主给出的具体原因，缺失才退回通用文案。
wxString ReadinessText(const submit::Readiness& readiness)
{
    if (!readiness.explanation.empty()) return Text(readiness.explanation);
    return Text(submit::RejectText(submit::Reject::NotReady));
}

/// 提交文本按 UTF-8 字节判定空白；长度上限属宿主合同，本层只做空白判定。
std::string Utf8Of(const wxString& value)
{
    const wxScopedCharBuffer utf8 = value.utf8_str();
    if (utf8.data() == nullptr) return std::string();
    return std::string(utf8.data(), utf8.length());
}

// ---------------------------------------------------------------------------
// 几何：输入壳 / 发送钮 / 原生输入控件
// ---------------------------------------------------------------------------

struct ComposerBox
{
    wxRect field;   ///< 输入壳（40 DIP 行，圆角与边线画在这里）
    wxRect send;    ///< 发送钮
    int padX = 0;
    int padTop = 0;
    int padBottom = 0;
    int gap = 0;
    int rowHeight = 0;
};

/// §4.6 `padding:11px 14px 13px` + `gap:8`；输入行高度取 Composer 的 minHeight（40 DIP）。
ComposerBox BoxFor(const wxWindow* window, int width, int rowHeight, int sendSize)
{
    ComposerBox box;
    box.padX = Dip(window, S::ComposePadX);
    box.padTop = Dip(window, S::ComposePadY);
    box.padBottom = Dip(window, S::ComposePadBottom);
    box.gap = Dip(window, S::ComposeGap);
    box.rowHeight = std::max(0, rowHeight);
    const int send = std::max(0, sendSize);
    const int content = std::max(0, width - 2 * box.padX);
    const int fieldWidth = std::max(0, content - send - box.gap);
    const int sendTop = box.padTop + std::max(0, (box.rowHeight - send) / 2);
    box.field = wxRect(box.padX, box.padTop, fieldWidth, box.rowHeight);
    box.send = wxRect(box.padX + fieldWidth + box.gap, sendTop, send, send);
    return box;
}

/// 原生输入控件的位置：外壳的 1 DIP 边线与左右文字内距（§4.6 H255 padding 13）都留在
/// 子窗口之外，因此面板画的圆角/边线/光晕不会被遮住。
wxRect InputControlRect(const wxWindow* window, const ComposerBox& box)
{
    const int border = std::max(1, Dip(window, ComposerTokens().borderDip));
    const int textPad = Dip(window, S::FieldPadX);
    wxRect rect = box.field;
    rect.Deflate(border + textPad, border);
    return rect;
}

// ---------------------------------------------------------------------------
// 绘制小工具（只在面板里画壳与环，不改为控件类；口径与 AgentControls 的内部实现一致）
// ---------------------------------------------------------------------------

double ClampRadius(double radius, const wxRect& rect)
{
    const double limit = std::min(rect.width, rect.height) / 2.0;
    return std::max(0.0, std::min(radius, limit));
}

/// 在 rect 边界内侧画一条宽 width 的环带（描边居中于内缩 width/2 的路径）。
void StrokeBand(wxGraphicsContext& gc, const wxRect& rect, double radius, int width,
                const wxColour& colour)
{
    if (width <= 0 || rect.width <= width || rect.height <= width) return;
    const double half = width / 2.0;
    wxGraphicsPath path = gc.CreatePath();
    path.AddRoundedRectangle(rect.x + half, rect.y + half, rect.width - width,
                             rect.height - width,
                             std::max(0.0, ClampRadius(radius, rect) - half));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour).Width(width)));
    gc.StrokePath(path);
}

size_t CountLines(const wxString& text)
{
    if (text.empty()) return 0;
    size_t lines = 1;
    for (wxString::const_iterator it = text.begin(); it != text.end(); ++it) {
        if (*it == '\n') ++lines;
    }
    return lines;
}

/// 把**未消费**的按键显式交给页面（Coordinator）。
/// 为什么不能只靠 event.Skip()：合成/投递到原生 wxTextCtrl 的 wxEVT_KEY_DOWN 只在本控件
/// 内处理，不会自动冒泡到父窗口，Esc 语义就永远到不了页面。这里显式上抛一次；
/// 页面处理器若把同一事件又送回输入区，用 thread_local 标记挡住，避免形成环。
void ForwardKeyToPage(wxWindow* owner, wxKeyEvent& event)
{
    static thread_local bool forwarding = false;
    if (forwarding || owner == nullptr) return;
    wxWindow* parent = owner->GetParent();
    if (parent == nullptr) return;
    forwarding = true;
    parent->GetEventHandler()->ProcessEvent(event);
    forwarding = false;
}

// ---------------------------------------------------------------------------
// 真实 IME 状态（只在 Windows 上有意义）
//
// 不新增链接依赖：不包含 <imm.h> 也不链 imm32.lib，而是一次性动态取入口
// （函数内静态，C++11 起初始化线程安全）。拿不到入口 = 当作"没有输入法"，
// 让 Enter 走正常提交路径，而不是让输入区永远不能发送。
// 故意不 FreeLibrary：IME 一旦被系统加载，进程存活期内保持加载才是安全做法。
// ---------------------------------------------------------------------------
#ifdef __WXMSW__
namespace {

/// imm.h 的 GCS_COMPSTR（取合成串）。只用一个常量，不复刻整个 imm.h。
constexpr DWORD kGcsCompStr = 0x0008;

using ImmGetContextFn = HANDLE(WINAPI*)(HWND);
using ImmReleaseContextFn = BOOL(WINAPI*)(HWND, HANDLE);
using ImmGetCompositionStringWFn = LONG(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD);

/// GetProcAddress 返回 FARPROC，与自定义函数指针类型直接互转会被 -Wcast-function-type
/// 判为不兼容转换；经 memcpy 取值是等价的、可移植的标准写法（尺寸有编译期断言）。
template <typename Entry>
Entry LoadEntry(HMODULE module, const char* name)
{
    static_assert(sizeof(Entry) == sizeof(FARPROC), "导出函数指针宽度应与 FARPROC 一致");
    Entry entry = nullptr;
    const FARPROC raw = ::GetProcAddress(module, name);
    if (raw != nullptr) std::memcpy(&entry, &raw, sizeof(entry));
    return entry;
}

struct ImmApi
{
    ImmGetContextFn getContext = nullptr;
    ImmReleaseContextFn releaseContext = nullptr;
    ImmGetCompositionStringWFn getCompositionString = nullptr;

    bool available() const
    {
        return getContext != nullptr && releaseContext != nullptr &&
               getCompositionString != nullptr;
    }
};

const ImmApi& Win32ImeApi()
{
    static const ImmApi api = [] {
        ImmApi loaded;
        const HMODULE module = ::LoadLibraryW(L"imm32.dll");
        if (module == nullptr) return loaded;
        loaded.getContext = LoadEntry<ImmGetContextFn>(module, "ImmGetContext");
        loaded.releaseContext = LoadEntry<ImmReleaseContextFn>(module, "ImmReleaseContext");
        loaded.getCompositionString =
            LoadEntry<ImmGetCompositionStringWFn>(module, "ImmGetCompositionStringW");
        return loaded;
    }();
    return api;
}

}  // namespace
#endif  // __WXMSW__

}  // namespace

// ===========================================================================
// 构造 / 析构
// ===========================================================================

AgentComposer::AgentComposer(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxBORDER_NONE | wxTAB_TRAVERSAL)
{
    SetName("agent-composer");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    // 存活令牌：宿主提交回调可能在同步栈里销毁本窗口（换页/关窗），回调返回后靠它判断
    // 是否还能碰成员。
    m_alive = std::make_shared<bool>(true);

    // 多行原生输入：Enter 由本组件的键处理决定是否提交，因此不需要 wxTE_PROCESS_ENTER；
    // 也不加 wxWANTS_CHARS（会挡住 Tab 遍历）。
    m_input = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize,
                             wxTE_MULTILINE | wxBORDER_NONE);
    m_input->SetName(Text("提问输入"));
    m_input->SetHint(Text(PlaceholderCopy()));

    m_send = new controls::AgentButton(this, wxID_ANY, wxString(), controls::Variant::Primary);
    // SetIcon 的入参是 DIP（AgentButton 内部再按 DPI 换算位图像素），所以直接给 token 值。
    m_send->SetIcon(icons::IconId::ComposerSend, ComposerTokens().iconDip);
    m_send->SetAccessibleName(Text("发送"));
    m_send->SetMinSize(wxSize(SendSizePx(), SendSizePx()));

    m_hint = new controls::AgentButton(this, wxID_ANY, Text("下一步提示"), controls::Variant::Ghost);
    m_finish = new controls::AgentButton(this, wxID_ANY, Text("完成学习"), controls::Variant::Ghost);
    m_cancel = new controls::AgentButton(this, wxID_ANY, Text("取消"), controls::Variant::Ghost);
    m_refresh = new controls::AgentButton(this, wxID_ANY, Text("刷新状态"), controls::Variant::Ghost);
    m_refreshEvidence = new controls::AgentButton(this, wxID_ANY, Text("刷新证据"), controls::Variant::Ghost);
    m_recover = new controls::AgentButton(this, wxID_ANY, Text("恢复教学"), controls::Variant::Ghost);
    for (auto* button : {m_hint, m_finish, m_cancel, m_refresh, m_refreshEvidence, m_recover})
        button->Show(false);
    m_hint->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { const auto action = m_workflowAction; if (action) action("hint_next"); });
    m_finish->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { const auto action = m_workflowAction; if (action) action("finish"); });
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { const auto action = m_workflowAction; if (action) action("cancel"); });
    m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { const auto action = m_workflowAction; if (action) action("refresh"); });
    // 刷新证据 = 只读 POST refresh_evidence（随后 GET 读状态）；恢复 = 仅 RecoveryRequired。
    m_refreshEvidence->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { const auto action = m_workflowAction; if (action) action("refresh_evidence"); });
    m_recover->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { const auto action = m_workflowAction; if (action) action("recover"); });
    m_refreshEvidence->SetToolTip(Text("重新读取后端证据（只读，不执行任何工具）"));
    m_recover->SetToolTip(Text("仅在服务端报告 RecoveryRequired 时可用"));

    m_feedback = new wxStaticText(this, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize,
                                  wxST_NO_AUTORESIZE);
    m_feedback->SetName(Text("发送反馈"));
    m_feedback->Show(false);

    // 首帧（宿主尚未调用 Arrange）也要有正确的行高与按钮尺寸。
    m_inputHeight = std::max(1, Dip(this, ComposerTokens().minHeightDip));
    m_sendSize = std::max(1, SendSizePx());

    Bind(wxEVT_PAINT, &AgentComposer::OnPaint, this);
    Bind(wxEVT_SIZE, &AgentComposer::OnSize, this);
    m_input->Bind(wxEVT_SET_FOCUS, &AgentComposer::OnInputFocus, this);
    m_input->Bind(wxEVT_KILL_FOCUS, &AgentComposer::OnInputFocus, this);
    m_input->Bind(wxEVT_KEY_DOWN, &AgentComposer::OnInputKeyDown, this);
    m_input->Bind(wxEVT_TEXT, &AgentComposer::OnInputText, this);
    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSendPressed(); });

    ApplyTheme();
    UpdateSendState();
    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
}

AgentComposer::~AgentComposer()
{
    // ① 先作废存活令牌：宿主回调若正处在同步栈里，返回后看到 false 就不再碰本对象。
    if (m_alive != nullptr) *m_alive = false;
    // ② 再解绑主题回调：子控件销毁期间不得再有 ApplyTheme 触碰它们。
    m_theme.Reset();
}

// ===========================================================================
// 提交通道
// ===========================================================================

void AgentComposer::SetSubmitCallback(SubmitCallback callback)
{
    m_submit = std::move(callback);
}

void AgentComposer::SetWorkflowActions(bool visible, bool hint, bool finish, bool cancel,
                                       std::function<void(const std::string&)> handler)
{
    m_workflowVisible = visible;
    m_workflowAction = std::move(handler);
    m_hint->Enable(hint);
    m_finish->Enable(finish);
    m_cancel->Enable(cancel);
    m_refresh->Enable(visible && static_cast<bool>(m_workflowAction));
    for (auto* button : {m_hint, m_finish, m_cancel, m_refresh}) button->Show(visible);
    UpdateRecoveryButtons();
}

void AgentComposer::SetRecoveryActions(bool refreshEvidenceVisible, bool recoverVisible,
                                       const wxString& recoverReason)
{
    m_refreshEvidenceRequested = refreshEvidenceVisible;
    m_recoverRequested = recoverVisible;
    m_recoverReason = recoverReason;
    UpdateRecoveryButtons();
}

void AgentComposer::UpdateRecoveryButtons()
{
    if (m_refreshEvidence == nullptr || m_recover == nullptr) return;
    const bool rowVisible = m_workflowVisible && static_cast<bool>(m_workflowAction);
    const bool showRefreshEvidence = rowVisible && m_refreshEvidenceRequested;
    const bool showRecover = rowVisible && m_recoverRequested;
    m_refreshEvidence->Show(showRefreshEvidence);
    m_refreshEvidence->Enable(showRefreshEvidence);
    m_recover->Show(showRecover);
    m_recover->Enable(showRecover);
    if (showRecover) {
        m_recover->SetDisabledReason(wxString());
    } else if (!m_recoverRequested && !m_recoverReason.empty()) {
        // 未显示时也保留明确原因，供工作台/测试读取（不伪造动作）。
        m_recover->SetDisabledReason(m_recoverReason);
    }
}

bool AgentComposer::RequestSubmit()
{
    ++m_attemptCount;

    if (m_busy) {
        ShowFeedback(BusyText());
        return false;
    }
    if (!m_readiness.ready) {
        ShowFeedback(ReadinessText(m_readiness));
        return false;
    }
    if (m_input == nullptr) return false;

    const wxString draft = m_input->GetValue();
    if (submit::IsBlank(Utf8Of(draft))) {
        ShowFeedback(EmptyText());
        return false;
    }

    // 宿主回调可能销毁本窗口：先取回调与存活令牌的副本，调用期间不再读成员；
    // 回调返回后若页面已被析构，立即返回，不得再碰任何成员。
    const SubmitCallback callback = m_submit;
    if (!callback) {
        ShowFeedback(ReadinessText(m_readiness));
        return false;
    }
    const std::shared_ptr<bool> alive = m_alive;
    const bool accepted = callback(draft);
    if (alive != nullptr && !*alive) return false;
    if (accepted) ++m_submitCount;
    return accepted;
}

bool AgentComposer::SubmitTrustedText(const wxString& text)
{
    ++m_attemptCount;

    if (m_busy) {
        ShowFeedback(BusyText());
        return false;
    }
    if (!m_readiness.ready) {
        ShowFeedback(ReadinessText(m_readiness));
        return false;
    }
    if (submit::IsBlank(Utf8Of(text))) {
        ShowFeedback(EmptyText());
        return false;
    }

    // trusted 文本只提交一次，**不改用户草稿**（用于已确认的外部提交；建议按钮仅填入草稿）。
    // 与 RequestSubmit 同一套存活令牌：回调里被析构时不再碰成员。
    const SubmitCallback callback = m_submit;
    if (!callback) {
        ShowFeedback(ReadinessText(m_readiness));
        return false;
    }
    const std::shared_ptr<bool> alive = m_alive;
    const bool accepted = callback(text);
    if (alive != nullptr && !*alive) return false;
    if (accepted) ++m_submitCount;
    return accepted;
}

// ===========================================================================
// 草稿
// ===========================================================================

void AgentComposer::SetDraftText(const wxString& text)
{
    if (m_input == nullptr) return;
    if (m_input->GetValue() == text) return;  // 同值不写，避免光标与选区跳动
    // ChangeValue 不发 wxEVT_TEXT：程序设置草稿不得被当成用户编辑。
    m_syncingDraft = true;
    m_input->ChangeValue(text);
    m_syncingDraft = false;
}

wxString AgentComposer::DraftText() const
{
    return m_input != nullptr ? m_input->GetValue() : wxString();
}

void AgentComposer::SetDraftEditedCallback(std::function<void(const wxString&)> callback)
{
    m_draftEdited = std::move(callback);
}

// ===========================================================================
// 状态
// ===========================================================================

void AgentComposer::SetBusy(bool busy)
{
    m_busy = busy;
    // busy 只禁用发送、不清草稿：用户仍可编辑下一份草稿。
    UpdateSendState();
}

void AgentComposer::SetReady(const submit::Readiness& readiness)
{
    m_readiness = readiness;
    UpdateSendState();
}

void AgentComposer::SetFeedback(const wxString& text)
{
    ShowFeedback(text);
}

wxString AgentComposer::FeedbackText() const
{
    return m_feedbackText;
}

bool AgentComposer::IsSendEnabled() const
{
    return m_send != nullptr && m_send->IsEnabled();
}

wxString AgentComposer::SendDisabledReason() const
{
    return m_send != nullptr ? m_send->DisabledReason() : wxString();
}

void AgentComposer::UpdateSendState()
{
    if (m_send == nullptr) return;
    if (m_readiness.ready && !m_busy) {
        // 清原因不会自动启用（AgentButton 的刻意语义），所以显式 Enable(true)。
        m_send->SetDisabledReason(wxString());
        m_send->Enable(true);
        return;
    }
    // busy 是当下真正挡住发送的原因；否则写门禁原因（缺失时用通用文案）。
    m_send->SetDisabledReason(m_busy ? BusyText() : ReadinessText(m_readiness));
}

// ===========================================================================
// 布局
// ===========================================================================

int AgentComposer::Arrange(int x, int y, int width)
{
    const int totalWidth = std::max(0, width);
    // 先写 m_width：本函数末尾的 SetSize 会同步触发 wxEVT_SIZE，OnSize 见到宽度未变即返回，
    // 因此不会递归重排。
    m_width = totalWidth;
    m_inputHeight = std::max(1, Dip(this, ComposerTokens().minHeightDip));
    m_sendSize = std::max(1, SendSizePx());

    const ComposerBox box = BoxFor(this, m_width, m_inputHeight, m_sendSize);
    const int gap = std::max(0, GapPx());
    int height = box.padTop + box.rowHeight + box.padBottom;

    // 反馈行：只有非空时才占高度；按新宽度重新折行（字体也可能刚被主题换过）。
    wxString feedbackLabel;
    int feedbackHeight = 0;
    size_t feedbackLines = 0;
    const bool wantsFeedback = m_feedback != nullptr && !m_feedbackText.empty();
    if (wantsFeedback) {
        const int available = std::max(1, m_width - 2 * box.padX);
        const tokens::WrappedText wrapped =
            tokens::WrapTextForWidth(this, m_feedback->GetFont(), m_feedbackText, available);
        feedbackLabel = wrapped.text;
        feedbackHeight = std::max(0, wrapped.height);
        feedbackLines = CountLines(wrapped.text);
        height += gap + feedbackHeight;
    }
    const int actionHeight = m_workflowVisible ? Dip(this, 32) : 0;
    const bool extraRowVisible = m_workflowVisible &&
                                 (m_refreshEvidenceRequested || m_recoverRequested);
    if (m_workflowVisible) height += gap + actionHeight;
    if (extraRowVisible) height += gap + actionHeight;
    m_feedbackLines = feedbackLines;
    m_height = std::max(0, height);

    if (m_input != nullptr) {
        const wxRect control = InputControlRect(this, box);
        m_input->SetSize(control.x, control.y, std::max(0, control.width),
                         std::max(0, control.height));
        m_input->Show(true);
    }
    if (m_send != nullptr) {
        m_send->SetSize(box.send.x, box.send.y, std::max(0, box.send.width),
                        std::max(0, box.send.height));
        m_send->Show(true);
    }
    if (m_feedback != nullptr) {
        if (wantsFeedback && feedbackHeight > 0) {
            m_feedback->SetLabel(feedbackLabel);
            m_feedback->SetSize(box.padX, box.padTop + box.rowHeight + gap,
                                std::max(0, m_width - 2 * box.padX), feedbackHeight);
            m_feedback->Show(true);
        } else {
            m_feedback->SetLabel(wxString());
            m_feedback->Show(false);
        }
    }
    if (m_workflowVisible) {
        const int top = box.padTop + box.rowHeight + gap +
                        (wantsFeedback ? feedbackHeight + gap : 0);
        const int available = std::max(0, m_width - 2 * box.padX);
        const int cell = std::max(0, (available - 3 * gap) / 4);
        controls::AgentButton* buttons[] = {m_hint, m_finish, m_cancel, m_refresh};
        for (int i = 0; i < 4; ++i) buttons[i]->SetSize(box.padX + i * (cell + gap), top, cell, actionHeight);
        if (extraRowVisible) {
            controls::AgentButton* extra[] = {m_refreshEvidence, m_recover};
            int shown = 0;
            for (auto* button : extra) if (button->IsShown()) ++shown;
            shown = std::max(1, shown);
            const int extraCell = std::max(0, (available - (shown - 1) * gap) / shown);
            int column = 0;
            for (auto* button : extra) {
                if (!button->IsShown()) continue;
                button->SetSize(box.padX + column * (extraCell + gap), top + actionHeight + gap,
                                extraCell, actionHeight);
                ++column;
            }
        }
    }

    SetSize(x, y, m_width, m_height);
    Refresh(false);
    return m_height;
}

void AgentComposer::ApplyTheme()
{
    const wxColour surface = tokens::SurfaceColour(tokens::Component::Card);
    SetBackgroundColour(surface);

    if (m_input != nullptr) {
        m_input->SetFont(InputFont());
        // 输入壳底色 = Composer surface（§4.6 H255 白 .78 预合成 glassInput），
        // 与面板画的壳同色，内缩接缝不可见。
        m_input->SetBackgroundColour(tokens::SurfaceColour(tokens::Component::Composer));
        m_input->SetForegroundColour(tokens::Colours().text1);
    }
    if (m_feedback != nullptr) {
        m_feedback->SetFont(FeedbackFont());
        m_feedback->SetForegroundColour(tokens::Colours().text2);
        m_feedback->SetBackgroundColour(surface);
    }

    if (m_width > 0) {
        // 字体与 DPI 都可能变了：按当前宽度重排（顺带按新度量重折反馈行）。
        const wxPoint position = GetPosition();
        Arrange(position.x, position.y, m_width);
        return;
    }
    Refresh(false);
}

// ===========================================================================
// 焦点
// ===========================================================================

void AgentComposer::FocusInput()
{
    if (m_input != nullptr) m_input->SetFocus();
}

bool AgentComposer::HasInputFocus() const
{
    return m_input != nullptr && m_input->HasFocus();
}

// ===========================================================================
// 键盘 / IME
// ===========================================================================

bool AgentComposer::EnterCommits(bool shiftDown, bool imeComposing)
{
    // Shift+Enter 换行；输入法合成中按 Enter 是"确认候选"，不得提交。
    return !shiftDown && !imeComposing;
}

bool AgentComposer::ImeComposing() const
{
    if (m_imeOverride.has_value()) return *m_imeOverride;
#ifdef __WXMSW__
    if (m_input == nullptr) return false;
    const ImmApi& api = Win32ImeApi();
    if (!api.available()) return false;  // 取不到 imm32：按"没有输入法"处理

    const HWND hwnd = static_cast<HWND>(m_input->GetHandle());
    if (hwnd == nullptr) return false;
    const HANDLE context = api.getContext(hwnd);
    if (context == nullptr) return false;  // 没有输入法上下文 = 不可能在合成

    // 合成串长度（UTF-16 字节数）> 0 才算正在合成：只有"输入法打开"而候选串为空时，
    // Enter 仍是普通提交，不能被误判成合成。
    const LONG length = api.getCompositionString(context, kGcsCompStr, nullptr, 0);
    api.releaseContext(hwnd, context);
    return length > 0;
#else
    return false;
#endif
}

void AgentComposer::SetImeComposingOverrideForTest(std::optional<bool> composing)
{
    m_imeOverride = composing;
}

// ===========================================================================
// 诊断
// ===========================================================================

wxWindow* AgentComposer::SendButtonWindow() const
{
    return m_send;
}

wxString AgentComposer::PlaceholderText() const
{
    return m_input != nullptr ? m_input->GetHint() : wxString();
}

// ===========================================================================
// 事件
// ===========================================================================

void AgentComposer::OnSize(wxSizeEvent& event)
{
    const int width = std::max(0, GetClientSize().x);
    if (m_input != nullptr && width > 0 && width != m_width) {
        // 宽度变了（宿主直接改尺寸或 sizer 生效）：按同一套 token 重排，
        // 反馈行必须按新宽度重新折行，否则高度会停在旧值上。
        const wxPoint position = GetPosition();
        Arrange(position.x, position.y, width);
    }
    event.Skip();
}

void AgentComposer::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    const wxSize size = GetClientSize();
    const wxColour background = tokens::SurfaceColour(tokens::Component::Card);
    dc.SetBackground(wxBrush(background));
    dc.Clear();
    if (size.x <= 0 || size.y <= 0) return;

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    const tokens::ComponentTokens& tab = ComposerTokens();
    const auto& colours = tokens::Colours();
    const int hairline = std::max(1, Dip(this, M::Hairline));
    const int border = std::max(1, Dip(this, tab.borderDip));
    const int glow = std::max(0, Dip(this, M::FocusGlow));

    // §4.6 顶部发丝线：输入区与上方消息区的分隔（1 DIP，随主题）。
    gc->SetPen(gc->CreatePen(
        wxGraphicsPenInfo(tokens::BorderColour(tokens::Component::Composer)).Width(hairline)));
    gc->StrokeLine(0.0, hairline / 2.0, static_cast<double>(size.x), hairline / 2.0);

    const ComposerBox box = BoxFor(this, size.x, m_inputHeight, m_sendSize);
    if (box.field.width <= 0 || box.field.height <= 0) return;

    const double radius = ClampRadius(Dip(this, tab.radiusDip), box.field);

    // 焦点环（H257）：3 DIP accent-soft 光晕画在输入壳外侧，1 DIP accent 边线画在壳边上。
    if (m_focusRing && glow > 0) {
        wxRect ring = box.field;
        ring.Inflate(glow, glow);
        StrokeBand(*gc, ring, radius + glow, glow, colours.accentSoft);
    }

    // 输入壳表面 + 1 DIP 边线。子窗口只盖住内缩区域，所以这里的圆角与边线始终可见。
    wxGraphicsPath path = gc->CreatePath();
    path.AddRoundedRectangle(box.field.x, box.field.y, box.field.width, box.field.height,
                             radius);
    gc->SetBrush(wxBrush(tokens::SurfaceColour(tokens::Component::Composer)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->FillPath(path);
    StrokeBand(*gc, box.field, radius, border,
               m_focusRing ? colours.accent : tokens::BorderColour(tokens::Component::Composer));
}

void AgentComposer::OnInputFocus(wxFocusEvent& event)
{
    const bool focused = event.GetEventType() == wxEVT_SET_FOCUS;
    if (m_focusRing != focused) {
        m_focusRing = focused;
        Refresh(false);
    }
    event.Skip();
}

void AgentComposer::OnInputKeyDown(wxKeyEvent& event)
{
    const int code = event.GetKeyCode();
    if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER) {
        if (EnterCommits(event.ShiftDown(), ImeComposing())) {
            // 提交并消费事件：多行控件因此不会插入换行。
            RequestSubmit();
            return;
        }
        // Shift+Enter 保留换行；IME 合成中把 Enter 交还给输入法确认候选。
        // 两条路径都不消费事件，且显式把未处理按键交给页面（合成事件不会自动冒泡）。
        event.Skip();
        ForwardKeyToPage(this, event);
        return;
    }
    if (code == WXK_ESCAPE) {
        // Esc 一律不消费也不提交：收成桌宠等语义归页面（Coordinator）。
        event.Skip();
        ForwardKeyToPage(this, event);
        return;
    }
    event.Skip();
}

void AgentComposer::OnInputText(wxCommandEvent& event)
{
    if (!m_syncingDraft && m_draftEdited && m_input != nullptr) {
        const auto callback = m_draftEdited;
        const wxString text = m_input->GetValue();
        callback(text);
    }
    event.Skip();
}

void AgentComposer::OnSendPressed()
{
    RequestSubmit();
}

// ===========================================================================
// 反馈与字体
// ===========================================================================

void AgentComposer::ShowFeedback(const wxString& text)
{
    m_feedbackText = text;
    if (m_feedback == nullptr) return;

    if (text.empty()) {
        m_feedback->SetLabel(wxString());
        m_feedbackLines = 0;
        m_feedback->Show(false);
    } else {
        // 真正的折行在 Arrange 里按当前宽度做（此时宽度可能还没确定）。
        m_feedback->SetLabel(text);
        m_feedbackLines = CountLines(text);
        m_feedback->Show(true);
    }

    if (m_width > 0) {
        const wxPoint position = GetPosition();
        Arrange(position.x, position.y, m_width);
        return;
    }
    Refresh(false);
}

wxFont AgentComposer::InputFont() const
{
    const tokens::ComponentTokens& tab = ComposerTokens();
    return tokens::UiFontPx(this, tab.fontSizePx, tab.weight);
}

wxFont AgentComposer::FeedbackFont() const
{
    // 本地说明属辅助文案：用比正文小一档的 BodyPx（11.5）而不是正文 13px。
    return tokens::UiFontPx(this, T::SmallBodyPx);
}

int AgentComposer::GapPx() const
{
    return Dip(this, S::ComposeGap);
}

int AgentComposer::SendSizePx() const
{
    return Dip(this, M::SendButtonSize);
}

}  // namespace agent::gui
