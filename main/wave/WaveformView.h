#pragma once

#include "../trace/TraceSource.h"
#include "WaveViewState.h"
#include "WaveformRenderData.h"
#include "WaveSession.h"

#include <functional>
#include <memory>

#include <wx/window.h>

namespace sigflow {
namespace wave {

class WaveformGLCanvas;
class WaveformMiniMap;

// 波形视图控件（T-W2）：
// - GL 可用时由 WaveformGLCanvas 渲染，否则软件渲染回退（T-W2-04）；
// - 缩放 / 平移 / 边沿跳转 / 首页尾跳转（T-W2-03）。
class WaveformView : public wxWindow {
public:
    WaveformView(wxWindow* parent, bool forceSoftware = false);
    ~WaveformView() override;

    void SetTraceSource(std::shared_ptr<sigflow::trace::TraceSource> source);
    void SetVisibleSignals(const std::vector<int>& signalIds);

    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    void PanTime(double viewportFrac);
    void JumpToEdge(bool forward);
    void JumpToTime(sigflow::trace::TimeValue t);

    // ── W3：播放头 / Marker / A-B / 事件 ──
    void SetEvents(const std::vector<WaveEvent>& events);
    const std::vector<WaveEvent>& Events() const { return m_state.events; }
    void AddMarkerAt(sigflow::trace::TimeValue t);
    void ClearMarkers();
    void SetAB(sigflow::trace::TimeValue a, sigflow::trace::TimeValue b);
    void ClearAB();
    void SetPlayhead(sigflow::trace::TimeValue t);
    void ClearPlayhead();
    bool HasPlayhead() const { return m_state.hasPlayhead; }
    sigflow::trace::TimeValue Playhead() const { return m_state.playhead; }
    std::string MeasurementText() const;
    void SetGlitchThreshold(sigflow::trace::TimeValue threshold)
    {
        m_glitchThreshold = threshold;
    }

    // ── W3：Compare 联动 / 会话 ──
    void SetOnViewChanged(std::function<void()> callback);
    void NotifyViewChanged();
    void ApplyLinkedWindow(const WaveformView* source);
    void ApplyLinkedPlayhead(const WaveformView* source);
    WaveSessionData CaptureSession() const;
    void ApplySession(const WaveSessionData& session);

    void SetTheme(WaveTheme theme);
    WaveTheme Theme() const { return m_state.theme; }

    bool UsingOpenGL() const;
    const WaveViewState& State() const { return m_state; }
    void ShowContextMenu(const wxPoint& point,
                         sigflow::trace::TimeValue a,
                         sigflow::trace::TimeValue b);

private:
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    void OnMouseDown(wxMouseEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnMouseUp(wxMouseEvent& event);
    void OnRightDown(wxMouseEvent& event);
    void OnRightUp(wxMouseEvent& event);
    void OnKeyDown(wxKeyEvent& event);
    void OnGlFailed();

    void RebuildGlCanvas();
    void RenderSoftware(wxDC& dc);
    void AutoSelectSignals();
    int ContentHeight() const;
    void RefreshMiniMap();

    WaveViewState m_state;
    std::shared_ptr<sigflow::trace::TraceSource> m_source;
    WaveformGLCanvas* m_glCanvas = nullptr;
    bool m_forceSoftware;
    bool m_dragging = false;
    bool m_ctrlDrag = false;
    bool m_rightDragging = false;
    int m_lastMouseX = 0;
    int m_dragStartX = 0;
    int m_rightStartX = 0;
    bool m_suppressNotify = false;
    std::function<void()> m_onViewChanged;
    sigflow::trace::TimeValue m_glitchThreshold = 2;
    WaveformMiniMap* m_miniMap = nullptr;
};

} // namespace wave
} // namespace sigflow
