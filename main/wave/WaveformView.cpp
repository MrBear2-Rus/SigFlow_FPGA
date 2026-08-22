#include "WaveformView.h"

#include "WaveAnalysis.h"
#include "WaveCompareHub.h"
#include "WaveformGLCanvas.h"
#include "WaveformTextLayer.h"

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/pen.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace sigflow {
namespace wave {

namespace {

constexpr std::size_t kMaxAutoSignals = 128;

wxColour ToWxColor(const WaveColor& color)
{
    return wxColour(static_cast<unsigned char>(color.r * 255.0f),
                    static_cast<unsigned char>(color.g * 255.0f),
                    static_cast<unsigned char>(color.b * 255.0f));
}

} // namespace

WaveformView::WaveformView(wxWindow* parent, bool forceSoftware)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
               wxWANTS_CHARS),
      m_forceSoftware(forceSoftware)
{
    WaveCompareHub::Register(this);
    SetBackgroundColour(wxColour(30, 30, 32));
    Bind(wxEVT_PAINT, &WaveformView::OnPaint, this);
    Bind(wxEVT_SIZE, &WaveformView::OnSize, this);
    Bind(wxEVT_MOUSEWHEEL, &WaveformView::OnMouseWheel, this);
    Bind(wxEVT_LEFT_DOWN, &WaveformView::OnMouseDown, this);
    Bind(wxEVT_MOTION, &WaveformView::OnMouseMove, this);
    Bind(wxEVT_LEFT_UP, &WaveformView::OnMouseUp, this);
    Bind(wxEVT_KEY_DOWN, &WaveformView::OnKeyDown, this);

    if (!m_forceSoftware) {
        RebuildGlCanvas();
    }
}

WaveformView::~WaveformView()
{
    WaveCompareHub::Unregister(this);
}

void WaveformView::RebuildGlCanvas()
{
    if (m_glCanvas) return;
    try {
        m_glCanvas = new WaveformGLCanvas(
            this, m_state, m_source,
            [this]() {
                if (wxTheApp) {
                    wxTheApp->CallAfter([this]() { OnGlFailed(); });
                }
            },
            [this]() { NotifyViewChanged(); });
    } catch (...) {
        m_glCanvas = nullptr;
    }
}

void WaveformView::OnGlFailed()
{
    if (!m_glCanvas) return;
    WaveformGLCanvas* canvas = m_glCanvas;
    m_glCanvas = nullptr;
    canvas->Destroy();
    Refresh();
}

bool WaveformView::UsingOpenGL() const
{
    return m_glCanvas != nullptr;
}

void WaveformView::SetTraceSource(std::shared_ptr<sigflow::trace::TraceSource> source)
{
    m_source = std::move(source);
    m_state = WaveViewState();
    m_state.visibleSignalIds.clear();
    if (m_source) {
        const sigflow::trace::TraceTimeRange range = m_source->TimeRange();
        m_state.maxTime = range.valid ? range.end : 0;
        AutoSelectSignals();
        WaveViewInteraction::Reset(m_state);
    }
    if (m_glCanvas) {
        // GL 画布引用 m_state / m_source，无需重建；仅需刷新
        m_glCanvas->Refresh();
    }
    Refresh();
}

void WaveformView::SetEvents(const std::vector<WaveEvent>& events)
{
    m_state.events = events;
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::AddMarkerAt(sigflow::trace::TimeValue t)
{
    WaveMarker marker;
    marker.time = t;
    marker.label = "M" + std::to_string(m_state.markers.size() + 1);
    m_state.markers.push_back(marker);
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
    NotifyViewChanged();
}

void WaveformView::ClearMarkers()
{
    m_state.markers.clear();
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
    NotifyViewChanged();
}

void WaveformView::SetAB(sigflow::trace::TimeValue a, sigflow::trace::TimeValue b)
{
    m_state.abA = a;
    m_state.abB = b;
    m_state.hasAB = true;
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
    NotifyViewChanged();
}

void WaveformView::ClearAB()
{
    m_state.hasAB = false;
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
    NotifyViewChanged();
}

void WaveformView::SetPlayhead(sigflow::trace::TimeValue t)
{
    m_state.playhead = t;
    m_state.hasPlayhead = true;
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
    NotifyViewChanged();
}

void WaveformView::ClearPlayhead()
{
    m_state.hasPlayhead = false;
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
    NotifyViewChanged();
}

std::string WaveformView::MeasurementText() const
{
    if (!m_state.hasAB || !m_source) return std::string();
    for (int signalId : m_state.visibleSignalIds) {
        const sigflow::trace::SignalInfo* signal = m_source->SignalById(signalId);
        if (!signal || signal->kind != sigflow::trace::SignalKind::Scalar) continue;
        WaveMeasurement measure;
        std::string error;
        if (!MeasureSignal(*m_source, *signal, m_state.abA, m_state.abB,
                           m_glitchThreshold, measure)) {
            return std::string();
        }
        char buffer[192];
        std::snprintf(buffer, sizeof(buffer),
                      "%s  ΔT=%llu ns  edges=%llu  x/z=%llu  glitch=%llu  "
                      "period=%.1f ns  duty=%.1f%%",
                      signal->name.c_str(),
                      static_cast<unsigned long long>(m_state.abB - m_state.abA),
                      static_cast<unsigned long long>(measure.edgeCount),
                      static_cast<unsigned long long>(measure.xzCount),
                      static_cast<unsigned long long>(measure.glitchCount),
                      measure.avgPeriod, measure.dutyCycle * 100.0);
        return std::string(buffer);
    }
    return std::string();
}

void WaveformView::SetOnViewChanged(std::function<void()> callback)
{
    m_onViewChanged = std::move(callback);
}

void WaveformView::NotifyViewChanged()
{
    if (m_suppressNotify) return;
    WaveCompareHub::OnViewChanged(this);
    if (m_onViewChanged) m_onViewChanged();
}

void WaveformView::ApplyLinkedWindow(const WaveformView* source)
{
    if (!source) return;
    m_suppressNotify = true;
    m_state.timeOffset = source->m_state.timeOffset;
    m_state.timeSpan = source->m_state.timeSpan;
    m_suppressNotify = false;
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::ApplyLinkedPlayhead(const WaveformView* source)
{
    if (!source) return;
    m_suppressNotify = true;
    m_state.playhead = source->m_state.playhead;
    m_state.hasPlayhead = source->m_state.hasPlayhead;
    m_suppressNotify = false;
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

WaveSessionData WaveformView::CaptureSession() const
{
    WaveSessionData session;
    session.sourcePath = m_source ? m_source->Path() : std::string();
    session.visibleSignalIds = m_state.visibleSignalIds;
    session.timeOffset = m_state.timeOffset;
    session.timeSpan = m_state.timeSpan;
    session.markers = m_state.markers;
    session.events = m_state.events;
    session.playhead = m_state.playhead;
    session.hasPlayhead = m_state.hasPlayhead;
    session.abA = m_state.abA;
    session.abB = m_state.abB;
    session.hasAB = m_state.hasAB;
    return session;
}

void WaveformView::ApplySession(const WaveSessionData& session)
{
    m_state.timeOffset = session.timeOffset;
    m_state.timeSpan = session.timeSpan;
    m_state.markers = session.markers;
    m_state.events = session.events;
    m_state.playhead = session.playhead;
    m_state.hasPlayhead = session.hasPlayhead;
    m_state.abA = session.abA;
    m_state.abB = session.abB;
    m_state.hasAB = session.hasAB;
    if (!session.visibleSignalIds.empty()) {
        m_state.visibleSignalIds = session.visibleSignalIds;
    }
    m_state.Clamp();
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::SetVisibleSignals(const std::vector<int>& signalIds)
{
    m_state.visibleSignalIds = signalIds;
    m_state.Clamp();
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::AutoSelectSignals()
{
    if (!m_source) return;
    const auto& signals = m_source->Signals();
    const std::size_t count = std::min(signals.size(), kMaxAutoSignals);
    m_state.visibleSignalIds.clear();
    m_state.visibleSignalIds.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        m_state.visibleSignalIds.push_back(signals[i].id);
    }
    m_state.Clamp();
}

void WaveformView::ZoomIn()
{
    WaveViewInteraction::ZoomStep(m_state, true);
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::ZoomOut()
{
    WaveViewInteraction::ZoomStep(m_state, false);
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::ZoomReset()
{
    WaveViewInteraction::Reset(m_state);
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::PanTime(double viewportFrac)
{
    WaveViewInteraction::PanBy(m_state, viewportFrac);
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::JumpToEdge(bool forward)
{
    if (!m_source || m_state.visibleSignalIds.empty()) return;
    const sigflow::trace::SignalInfo* signal =
        m_source->SignalById(m_state.visibleSignalIds.front());
    if (!signal) return;

    std::vector<sigflow::trace::Transition> transitions;
    std::string error;
    if (forward) {
        const sigflow::trace::TimeValue from = m_state.EndTime();
        if (m_source->Query(*signal, from, from + m_state.timeSpan, transitions, error) &&
            !transitions.empty()) {
            WaveViewInteraction::JumpToTime(m_state, transitions.front().time);
        }
    } else {
        const sigflow::trace::TimeValue to = m_state.timeOffset;
        if (m_source->Query(*signal, 0, to, transitions, error) && !transitions.empty()) {
            WaveViewInteraction::JumpToTime(m_state, transitions.back().time);
        }
    }
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::JumpToTime(sigflow::trace::TimeValue t)
{
    WaveViewInteraction::JumpToTime(m_state, t);
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
    NotifyViewChanged();
}

void WaveformView::OnSize(wxSizeEvent& event)
{
    if (m_glCanvas) {
        m_glCanvas->SetSize(GetClientSize());
    }
    event.Skip();
}

void WaveformView::OnPaint(wxPaintEvent&)
{
    if (m_glCanvas) return; // GL 子窗口负责绘制
    wxPaintDC dc(this);
    RenderSoftware(dc);
}

void WaveformView::RenderSoftware(wxDC& dc)
{
    const wxSize size = GetClientSize();
    dc.SetBackground(wxBrush(wxColour(30, 30, 32)));
    dc.Clear();
    if (!m_source) {
        dc.SetTextForeground(wxColour(148, 163, 184));
        dc.DrawText("Open a trace file to view waveforms.", 12, 12);
        return;
    }
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0) return;

    m_state.Clamp();
    WaveformFrame frame;
    std::string error;
    if (!BuildWaveformFrame(*m_source, m_state, size.GetWidth(), size.GetHeight(), frame)) {
        dc.SetTextForeground(wxColour(239, 68, 68));
        dc.DrawText(error, 12, 12);
        return;
    }

    // 网格
    dc.SetPen(wxPen(wxColour(45, 45, 50)));
    for (const WaveformTick& tick : frame.ticks) {
        const int x = static_cast<int>(
            WaveformTimeToX(m_state, size.GetWidth(), tick.time));
        dc.DrawLine(x, m_state.headerHeight, x, size.GetHeight());
    }

    // 波形
    int row = 0;
    for (const WaveformSignalData& signal : frame.signals) {
        dc.SetPen(wxPen(ToWxColor(WavePalette(row)), 1));
        std::vector<wxPoint> points;
        points.reserve(signal.polyline.size());
        for (const WaveformPoint& point : signal.polyline) {
            points.push_back(wxPoint(static_cast<int>(point.x),
                                     static_cast<int>(point.y)));
        }
        if (points.size() >= 2) dc.DrawLines(points.size(), points.data());
        ++row;
    }

    // ── W3 覆盖层：A-B / 播放头 / Marker / 事件 ──
    const int plotRight = size.GetWidth() - m_state.rightMargin;
    if (m_state.hasAB) {
        const int xa = static_cast<int>(
            WaveformTimeToX(m_state, size.GetWidth(), m_state.abA));
        const int xb = static_cast<int>(
            WaveformTimeToX(m_state, size.GetWidth(), m_state.abB));
        dc.SetPen(wxPen(wxColour(251, 146, 60), 1));
        dc.DrawLine(xa, m_state.headerHeight, xa, size.GetHeight());
        dc.DrawLine(xb, m_state.headerHeight, xb, size.GetHeight());
    }
    if (m_state.hasPlayhead) {
        const int xp = static_cast<int>(
            WaveformTimeToX(m_state, size.GetWidth(), m_state.playhead));
        dc.SetPen(wxPen(wxColour(226, 232, 240), 1));
        dc.DrawLine(xp, m_state.headerHeight, xp, size.GetHeight());
    }
    for (const WaveMarker& marker : m_state.markers) {
        const int x = static_cast<int>(
            WaveformTimeToX(m_state, size.GetWidth(), marker.time));
        const wxColour color((marker.color >> 16) & 0xFF, (marker.color >> 8) & 0xFF,
                             marker.color & 0xFF);
        dc.SetPen(wxPen(color, 1));
        dc.DrawLine(x, m_state.headerHeight, x, size.GetHeight());
    }
    for (const WaveEvent& event : m_state.events) {
        const int x = static_cast<int>(
            WaveformTimeToX(m_state, size.GetWidth(), event.time));
        const wxColour color((event.color >> 16) & 0xFF, (event.color >> 8) & 0xFF,
                             event.color & 0xFF);
        dc.SetPen(wxPen(color, 1));
        dc.DrawLine(x, m_state.headerHeight, x, size.GetHeight());
    }

    // 文字层
    wxBitmap textBitmap;
    if (BuildTextLayerBitmap(this, frame, m_state, size.GetWidth(), size.GetHeight(),
                             textBitmap) &&
        textBitmap.IsOk()) {
        dc.DrawBitmap(textBitmap, 0, 0, true);
    }
}

void WaveformView::OnMouseWheel(wxMouseEvent& event)
{
    const wxSize size = GetClientSize();
    const int plotWidth = size.GetWidth() - m_state.leftMargin - m_state.rightMargin;
    const double frac = plotWidth > 0
        ? static_cast<double>(event.GetX() - m_state.leftMargin) / plotWidth
        : 0.5;
    WaveViewInteraction::ZoomAt(m_state, frac, event.GetWheelRotation() > 0 ? 0.7 : 1.45);
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::OnMouseDown(wxMouseEvent& event)
{
    m_dragging = true;
    m_lastMouseX = event.GetX();
    m_dragStartX = event.GetX();
    m_ctrlDrag = event.ControlDown();
    SetFocus();
}

void WaveformView::OnMouseMove(wxMouseEvent& event)
{
    const wxSize size = GetClientSize();
    const int plotWidth = size.GetWidth() - m_state.leftMargin - m_state.rightMargin;
    if (plotWidth <= 0) return;
    if (!m_dragging) return;
    if (m_ctrlDrag) {
        const sigflow::trace::TimeValue b =
            WaveViewInteraction::TimeAtX(m_state, size.GetWidth(), event.GetX());
        if (!m_state.hasAB) {
            m_state.abA = WaveViewInteraction::TimeAtX(
                m_state, size.GetWidth(), m_dragStartX);
            m_state.hasAB = true;
        }
        m_state.abB = b;
        if (m_glCanvas) m_glCanvas->Refresh();
        Refresh();
        return;
    }
    const double frac = static_cast<double>(event.GetX() - m_lastMouseX) / plotWidth;
    WaveViewInteraction::PanBy(m_state, -frac);
    m_lastMouseX = event.GetX();
    if (m_glCanvas) m_glCanvas->Refresh();
    Refresh();
}

void WaveformView::OnMouseUp(wxMouseEvent&)
{
    const bool wasCtrl = m_ctrlDrag;
    const bool wasDrag = m_dragging;
    const bool moved = std::abs(m_lastMouseX - m_dragStartX) >= 3;
    m_dragging = false;
    m_ctrlDrag = false;
    if (!wasDrag) return;
    const wxSize size = GetClientSize();
    if (wasCtrl) {
        if (!moved) {
            ClearAB();
        } else {
            NotifyViewChanged();
        }
        return;
    }
    if (moved) {
        NotifyViewChanged();
        return;
    }
    // 单击：Shift 放置 Marker，否则放置播放头
    const sigflow::trace::TimeValue t =
        WaveViewInteraction::TimeAtX(m_state, size.GetWidth(), m_dragStartX);
    if (wxGetKeyState(WXK_SHIFT)) {
        AddMarkerAt(t);
    } else {
        SetPlayhead(t);
    }
}

void WaveformView::OnKeyDown(wxKeyEvent& event)
{
    const int key = event.GetKeyCode();
    if (key == '+' || key == WXK_ADD || key == '=') {
        ZoomIn();
    } else if (key == '-' || key == WXK_SUBTRACT) {
        ZoomOut();
    } else if (key == '0') {
        ZoomReset();
    } else if (key == WXK_LEFT) {
        PanTime(-0.1);
    } else if (key == WXK_RIGHT) {
        PanTime(0.1);
    } else if (key == WXK_HOME) {
        WaveViewInteraction::JumpToTime(m_state, 0);
        if (m_glCanvas) m_glCanvas->Refresh();
        Refresh();
    } else if (key == WXK_END) {
        WaveViewInteraction::JumpToTime(m_state, m_state.maxTime);
        if (m_glCanvas) m_glCanvas->Refresh();
        Refresh();
    } else if (key == ',' ) {
        JumpToEdge(false);
    } else if (key == '.') {
        JumpToEdge(true);
    } else if (key == WXK_ESCAPE) {
        ClearAB();
        ClearPlayhead();
    } else {
        event.Skip();
        return;
    }
    NotifyViewChanged();
}

} // namespace wave
} // namespace sigflow
