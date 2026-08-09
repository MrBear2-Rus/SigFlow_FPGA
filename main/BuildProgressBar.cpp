// BuildProgressBar.cpp
// VS 风格编译进度条实现

#include "BuildProgressBar.h"

wxBEGIN_EVENT_TABLE(BuildProgressBar, wxPanel)
    EVT_TIMER(wxID_ANY, BuildProgressBar::OnTimer)
wxEND_EVENT_TABLE()

BuildProgressBar::BuildProgressBar(wxWindow* parent, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxSize(-1, 28)),
      m_successColour(0, 180, 80),    // VS green
      m_failColour(220, 50, 50)       // VS red
{
    // 面板背景：VS 状态栏灰
    SetBackgroundColour(wxColour(245, 245, 250));

    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);

    // 阶段标签
    m_stageLabel = new wxStaticText(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_stageLabel->SetFont(
        m_stageLabel->GetFont().Scale(0.85).MakeSmaller());
    m_stageLabel->SetForegroundColour(wxColour(80, 80, 90));
    sizer->Add(m_stageLabel, wxSizerFlags(1).CentreVertical().Border(wxLEFT, 8));

    // 进度条
    m_gauge = new wxGauge(this, wxID_ANY, 100,
        wxDefaultPosition, wxSize(180, FromDIP(10)),
        wxGA_HORIZONTAL | wxGA_SMOOTH);
    sizer->Add(m_gauge, wxSizerFlags(0).CentreVertical().Border(wxLEFT | wxRIGHT, 8));

    // 耗时标签
    m_elapsedLabel = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_elapsedLabel->SetFont(
        m_elapsedLabel->GetFont().Scale(0.85).MakeSmaller());
    m_elapsedLabel->SetForegroundColour(wxColour(120, 120, 130));
    sizer->Add(m_elapsedLabel, wxSizerFlags(0).CentreVertical());

    // 取消按钮
    m_cancelBtn = new wxButton(this, wxID_CANCEL, wxT("✕"),
        wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)),
        wxBU_EXACTFIT | wxBORDER_NONE);
    m_cancelBtn->SetFont(m_cancelBtn->GetFont().Scale(0.8));
    m_cancelBtn->SetForegroundColour(wxColour(140, 140, 150));
    m_cancelBtn->SetBackgroundColour(GetBackgroundColour());
    m_cancelBtn->Bind(wxEVT_BUTTON, &BuildProgressBar::OnCancel, this);
    sizer->Add(m_cancelBtn, wxSizerFlags(0).CentreVertical().Border(wxRIGHT, 4));

    SetSizer(sizer);
    m_timer.SetOwner(this);
    Hide();
}

void BuildProgressBar::BeginOperation(const wxString& toolName, int totalStages)
{
    // 重入保护：若已有运行中的操作，先停止旧操作
    if (m_timer.IsRunning()) {
        m_timer.Stop();
    }

    m_toolName = toolName;
    m_totalStages = totalStages;
    m_currentStage = 0;
    m_running = true;
    m_finished = false;
    m_autoHideCountdown = 0;
    m_pulseDir = 1;

    // 标签
    m_stageLabel->SetLabel(toolName + wxT(" — starting..."));
    m_stageLabel->SetForegroundColour(wxColour(80, 80, 90));

    // Gauge：确定/脉冲模式
    m_gauge->SetRange(totalStages > 0 ? totalStages : 100);
    m_gauge->SetValue(0);
    // 注意：不在确定模式下调用 Pulse()，避免 indeterminate→determinate 切换闪烁

    // 取消按钮：显示+启用
    m_cancelBtn->Show();
    m_cancelBtn->SetForegroundColour(wxColour(140, 140, 150));
    m_cancelBtn->Enable(true);

    // 面板：显示
    SetBackgroundColour(wxColour(245, 245, 250));
    Show();
    if (m_onVisibility) m_onVisibility(true);
    if (GetParent()) GetParent()->Layout();

    m_stopWatch.Start();
    m_timer.Start(100);
}

void BuildProgressBar::AdvanceStage(const wxString& stageName, int currentStage)
{
    if (!m_running || m_finished) return;

    if (currentStage >= 0) {
        m_currentStage = currentStage;
    } else {
        ++m_currentStage;
    }

    m_stageLabel->SetLabel(m_toolName + wxT(" — ") + stageName);

    if (m_totalStages > 0 && m_currentStage <= m_totalStages) {
        m_gauge->SetValue(m_currentStage);
    }
}

void BuildProgressBar::Pulse(const wxString& statusText)
{
    if (!m_running || m_finished) return;

    if (!statusText.IsEmpty()) {
        m_stageLabel->SetLabel(m_toolName + wxT(" — ") + statusText);
    }

    // 手动模拟脉冲（仅在无明确阶段的脉冲模式下）
    if (m_totalStages == 0) {
        int val = m_gauge->GetValue();
        val += m_pulseDir * 8;
        if (val >= 100) { val = 100; m_pulseDir = -1; }
        if (val <= 0)   { val = 0;   m_pulseDir = 1;  }
        m_gauge->SetValue(val);
    }
}

void BuildProgressBar::FinishOperation(bool success, const wxString& message)
{
    if (!m_running) return;
    m_running = false;
    m_finished = true;
    m_timer.Stop();

    m_elapsedLabel->SetLabel(FormatElapsed());
    SetFinishedStyle(success);

    if (!message.IsEmpty()) {
        m_stageLabel->SetLabel(message);
    }

    // 成功：3 秒后自动隐藏；失败：保持显示
    if (success) {
        m_autoHideCountdown = 30;  // 30 × 100ms
        m_timer.Start(100);
    }
}

void BuildProgressBar::SetFinishedStyle(bool success)
{
    if (success) {
        wxString label = m_stageLabel->GetLabel();
        if (!label.StartsWith(wxT("✓ "))) {
            m_stageLabel->SetLabel(wxT("✓ ") + label);
        }
        m_stageLabel->SetForegroundColour(m_successColour);
        m_gauge->SetValue(m_totalStages > 0 ? m_totalStages : 100);
        SetBackgroundColour(wxColour(235, 250, 235));
    } else {
        wxString label = m_stageLabel->GetLabel();
        if (!label.StartsWith(wxT("✗ "))) {
            m_stageLabel->SetLabel(wxT("✗ ") + label);
        }
        m_stageLabel->SetForegroundColour(m_failColour);
        // 失败时 gauge 保持在当前位置不做无意义赋值
        SetBackgroundColour(wxColour(255, 240, 240));
    }
    m_cancelBtn->Hide();
    Refresh();
    if (GetParent()) GetParent()->Layout();
}

void BuildProgressBar::OnTimer(wxTimerEvent&)
{
    if (m_finished && m_autoHideCountdown > 0) {
        // 成功 / 取消 态倒计时自动隐藏
        --m_autoHideCountdown;
        if (m_autoHideCountdown == 0) {
            m_timer.Stop();
            Hide();
            if (m_onVisibility) m_onVisibility(false);
            if (GetParent()) GetParent()->Layout();
        }
        return;
    }

    if (!m_running) return;

    // 运行中：更新耗时
    m_elapsedLabel->SetLabel(FormatElapsed());

    // 无总阶段数时持续脉冲
    if (m_totalStages == 0) {
        Pulse();
    }
}

void BuildProgressBar::OnCancel(wxCommandEvent&)
{
    if (!m_running) return;

    m_running = false;
    m_finished = true;
    m_timer.Stop();

    m_stageLabel->SetLabel(m_toolName + wxT(" — cancelled"));
    m_stageLabel->SetForegroundColour(wxColour(180, 150, 0));
    SetBackgroundColour(wxColour(255, 252, 235));
    m_elapsedLabel->SetLabel(FormatElapsed());
    m_cancelBtn->Hide();
    Refresh();
    if (GetParent()) GetParent()->Layout();

    if (m_onCancel) {
        m_onCancel();
    }

    // 取消后 2 秒自动隐藏（让用户看到 cancelled 状态）
    m_autoHideCountdown = 20;
    m_timer.Start(100);
    if (m_onVisibility) m_onVisibility(true);
}

wxString BuildProgressBar::FormatElapsed()
{
    long ms = m_stopWatch.Time();
    if (ms < 1000) {
        return wxString::Format(wxT("%ldms"), ms);
    }
    if (ms < 60000) {
        return wxString::Format(wxT("%.1fs"), ms / 1000.0);
    }
    long minutes = ms / 60000;
    double seconds = (ms % 60000) / 1000.0;
    return wxString::Format(wxT("%ldm %.0fs"), minutes, seconds);
}
