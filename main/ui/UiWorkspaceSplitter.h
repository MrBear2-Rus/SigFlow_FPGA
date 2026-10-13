#pragma once
#include <wx/splitter.h>
#include <wx/dc.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include "ui/UiFocusTrace.h"
#include "ui/UiTheme.h"

// INT-009：保留 wxSplitter 原生拖拽；仅扩展把手反馈与聚焦后的方向键。
// KEY-008（T-620）：
//   ↑/↓ = 调整中区分栏（每次 3%，LAY-010 夹取 25–80%，LAY-011 两个窗格都 ≥50 DIP）；
//   ←/→ = 调整主侧栏宽度（每次 12 DIP，AUI pane 自身的 210–460 范围由 MainFrame 夹取）。
class UiWorkspaceSplitter final : public wxSplitterWindow {
public:
    using wxSplitterWindow::wxSplitterWindow;
    bool AcceptsFocus() const override { return true; }
    void SetFocus() override { SetFocusIgnoringChildren(); }

    /// 主侧栏宽度步进回调（step = -1 / +1，每次 12 DIP）。由 MainFrame 提供，
    /// 因为 AUI pane 的尺寸与最小/最大保护都归主窗所有。
    void SetSideBarStepHandler(std::function<void(int)> handler) { m_sideBarStep = std::move(handler); }

    void EnableAccessibleSash()
    {
        SetName("WorkspaceSplitter");
        Bind(wxEVT_MOTION,[this](wxMouseEvent& e) {
            UiFocusTrace::NotePointer();
            const bool hover=SashHitTest(e.GetX(),e.GetY());
            if (hover!=m_hover) { m_hover=hover; Refresh(false); }
            e.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW,[this](wxMouseEvent& e) { UiFocusTrace::NotePointer(); m_hover=false; Refresh(false); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent& e) {
            UiFocusTrace::NotePointer();
            if (SashHitTest(e.GetX(),e.GetY())) { SetFocus(); m_drag=true; Refresh(false); }
            e.Skip();
        });
        Bind(wxEVT_LEFT_UP,[this](wxMouseEvent& e) { m_drag=false; Refresh(false); e.Skip(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent& e) { m_drag=false; Refresh(false); e.Skip(); });
        Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& e) {
            UiFocusTrace::NoteKeyboard();
            const int key=e.GetKeyCode();
            if (wxWindow::FindFocus()!=this) { e.Skip(); return; }
            if (key==WXK_LEFT || key==WXK_RIGHT) {
                // KEY-008：←/→ 每次 12 DIP 调整主侧栏；夹取由 MainFrame 负责。
                if (m_sideBarStep) { m_sideBarStep(key==WXK_LEFT?-1:1); Refresh(false); }
                else e.Skip();
                return;
            }
            if (key!=WXK_UP && key!=WXK_DOWN) { e.Skip(); return; }
            if (!IsSplit()) { e.Skip(); return; }
            const int total=GetClientSize().y;
            if (total<=0) return;
            // INT-009：每次 3%；同时遵守 LAY-010（25%–80%）与 LAY-011（两侧窗格 ≥ 最小尺寸）。
            const int step=std::max(1,int(std::lround(total*0.03)));
            const int minPane=std::max(1,GetMinimumPaneSize());
            const int lower=std::max(total*25/100,minPane);
            const int upper=std::max(lower,std::min(total*80/100,total-minPane));
            const int next=std::clamp(GetSashPosition()+(key==WXK_UP?-step:step),lower,upper);
            SetSashPosition(next); Refresh(false);
            wxSplitterEvent changed(wxEVT_SPLITTER_SASH_POS_CHANGED,this);
            changed.SetEventObject(this);changed.SetSashPosition(next);ProcessWindowEvent(changed);
        });
    }
protected:
    void DrawSash(wxDC& dc) override
    {
        const auto& c=UiTheme::Palette();
        // A11Y-001：键盘聚焦时 sash 用 accent 合成色提示（pane sash 不受 DEC-14 局部规则约束）。
        const bool keyboardFocus=HasFocus();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_drag?c.accent:((m_hover||keyboardFocus)?c.focusRingOnPanel:c.strokeOnPanel)));
        dc.DrawRectangle(0,GetSashPosition(),GetClientSize().x,GetSashSize());
    }
private:
    bool m_hover=false,m_drag=false;
    std::function<void(int)> m_sideBarStep;
};
