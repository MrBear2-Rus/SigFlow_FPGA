// BuildProgressBar.h
// VS 风格编译进度条 — 松耦合、独立 wxPanel
// 不依赖任何 FPGA/EDA 概念，可复用于任意长时操作
#pragma once

#include <wx/wx.h>
#include <wx/gauge.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <wx/stopwatch.h>
#include <functional>

// VS 风格编译进度条
// ┌──────────────────────────────────────────────────────────┐
// │ ✓ Yosys Synthesis — Technology Mapping    [=====]  12.3s │
// └──────────────────────────────────────────────────────────┘
//
// 三种状态:
//   Hidden   — 无操作进行中，Hide()
//   Running  — BeginOperation() → AdvanceStage/Pulse → FinishOperation()
//   Finished — 成功：绿色✓ + 3秒自动隐藏 / 失败：红色✗ + 保持显示
class BuildProgressBar : public wxPanel
{
public:
    BuildProgressBar(wxWindow* parent, wxWindowID id = wxID_ANY);

    // ── 操作生命周期 ──
    // 开始一个操作。totalStages>0 时 gauge 为确定模式，=0 时脉冲模式
    void BeginOperation(const wxString& toolName, int totalStages = 0);

    // 推进到指定阶段（仅 totalStages>0 时有效）
    void AdvanceStage(const wxString& stageName, int currentStage = -1);

    // 脉冲一次（用于无明确阶段的输出流），更新状态文本
    void Pulse(const wxString& statusText = {});

    // 结束操作
    // success=true  → 绿色 ✓ + 3秒后自动隐藏
    // success=false → 红色 ✗ + 保持显示
    void FinishOperation(bool success, const wxString& message = {});

    // ── 取消支持 ──
    using CancelCallback = std::function<void()>;
    void SetCancelCallback(CancelCallback cb) { m_onCancel = std::move(cb); }
    bool IsRunning() const { return m_running; }

    // ── 可见性回调（供宿主控制容器显隐，如 wxAUI pane Show/Hide）──
    // BuildProgressBar 自身不依赖 wxAUI，通过此回调解耦
    using VisibilityCallback = std::function<void(bool visible)>;
    void SetVisibilityCallback(VisibilityCallback cb) { m_onVisibility = std::move(cb); }

private:
    void OnTimer(wxTimerEvent&);
    void OnCancel(wxCommandEvent&);
    void SetFinishedStyle(bool success);
    wxString FormatElapsed();

    // ── UI 控件 ──
    wxStaticText*   m_stageLabel;      // "Yosys Synthesis — ABC Optimization"
    wxGauge*        m_gauge;           // 进度条本体
    wxStaticText*   m_elapsedLabel;    // "12.3s"
    wxButton*       m_cancelBtn;       // ✕

    // ── 状态 ──
    wxTimer         m_timer;           // 100ms 计时+脉冲+自动隐藏
    wxStopWatch     m_stopWatch;        // 计时
    CancelCallback  m_onCancel;
    VisibilityCallback m_onVisibility;  // 容器显隐回调
    wxString        m_toolName;
    int             m_totalStages = 0;
    int             m_currentStage = 0;
    bool            m_running = false;
    bool            m_finished = false;
    int             m_autoHideCountdown = 0;   // 成功态倒计时（100ms为单位）
    int             m_pulseDir = 1;            // 脉冲方向
    wxColour        m_successColour;           // 绿色
    wxColour        m_failColour;              // 红色

    wxDECLARE_EVENT_TABLE();
};
