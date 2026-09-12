#include "WaveformGLCanvas.h"

#include "WaveformRenderData.h"
#include "WaveformTextLayer.h"

#include <wx/dcmemory.h>
#include <wx/dcclient.h>
#include <wx/image.h>

#include <cstdlib>
#include <vector>

namespace sigflow {
namespace wave {

namespace {

void BitmapToRgba(const wxBitmap& bitmap, int width, int height,
                  std::vector<std::uint8_t>& rgba)
{
    rgba.assign(static_cast<std::size_t>(width) * height * 4, 255);
    const wxImage image = bitmap.ConvertToImage();
    if (!image.IsOk() || image.GetWidth() != width || image.GetHeight() != height) {
        return;
    }
    const unsigned char* rgb = image.GetData();
    const unsigned char* alpha = image.GetAlpha();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t src = static_cast<std::size_t>(y * width + x) * 3;
            const std::size_t dst = static_cast<std::size_t>(y * width + x) * 4;
            rgba[dst] = rgb[src];
            rgba[dst + 1] = rgb[src + 1];
            rgba[dst + 2] = rgb[src + 2];
            rgba[dst + 3] = alpha ? alpha[y * width + x] : 255;
        }
    }
}

} // namespace

WaveformGLCanvas::WaveformGLCanvas(
    wxWindow* parent, WaveViewState& state,
    std::shared_ptr<sigflow::trace::TraceSource>& source,
    std::function<void()> onGlFailed,
    std::function<void()> onViewChanged,
    std::function<void(const wxPoint&, sigflow::trace::TimeValue,
                       sigflow::trace::TimeValue)> onContextMenu)
    : wxGLCanvas(parent, wxID_ANY, nullptr, wxDefaultPosition, wxDefaultSize,
                 wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS, wxGLCanvasName),
      m_state(state),
      m_source(source),
      m_onGlFailed(std::move(onGlFailed)),
      m_onViewChanged(std::move(onViewChanged)),
      m_onContextMenu(std::move(onContextMenu))
{
    Bind(wxEVT_PAINT, &WaveformGLCanvas::OnPaint, this);
    Bind(wxEVT_SIZE, &WaveformGLCanvas::OnSize, this);
    Bind(wxEVT_MOUSEWHEEL, &WaveformGLCanvas::OnMouseWheel, this);
    Bind(wxEVT_LEFT_DOWN, &WaveformGLCanvas::OnMouseDown, this);
    Bind(wxEVT_MOTION, &WaveformGLCanvas::OnMouseMove, this);
    Bind(wxEVT_LEFT_UP, &WaveformGLCanvas::OnMouseUp, this);
    Bind(wxEVT_RIGHT_DOWN, &WaveformGLCanvas::OnMouseDown, this);
    Bind(wxEVT_RIGHT_UP, &WaveformGLCanvas::OnMouseUp, this);
    Bind(wxEVT_KEY_DOWN, &WaveformGLCanvas::OnKeyDown, this);
    SetFocus();
}

WaveformGLCanvas::~WaveformGLCanvas()
{
    if (m_glContext) {
        SetCurrent(*m_glContext);
        m_renderer.Shutdown();
        delete m_glContext;
        m_glContext = nullptr;
    }
}

bool WaveformGLCanvas::EnsureContext()
{
    if (m_glContext) return m_glReady;
    try {
        m_glContext = new wxGLContext(this);
    } catch (...) {
        m_glContext = nullptr;
    }
    if (!m_glContext) {
        NotifyFailure();
        return false;
    }
    SetCurrent(*m_glContext);
    m_glReady = m_renderer.Initialize();
    if (!m_glReady) NotifyFailure();
    return m_glReady;
}

void WaveformGLCanvas::NotifyFailure()
{
    if (m_onGlFailed) {
        m_onGlFailed();
    }
}

void WaveformGLCanvas::OnSize(wxSizeEvent& event)
{
    event.Skip();
}

void WaveformGLCanvas::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    if (!EnsureContext()) return;
    RenderFrame();
}

void WaveformGLCanvas::RenderFrame()
{
    const wxSize size = GetClientSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0) return;
    if (!m_source) {
        const wxColour bg = ColorsFor(m_state.theme).background;
        m_renderer.BeginFrame(size.GetWidth(), size.GetHeight(),
                              { bg.Red() / 255.0f, bg.Green() / 255.0f,
                                bg.Blue() / 255.0f });
        m_renderer.EndFrame();
        SwapBuffers();
        return;
    }

    m_state.Clamp();
    WaveformFrame frame;
    std::string error;
    if (!BuildWaveformFrame(*m_source, m_state, size.GetWidth(), size.GetHeight(), frame)) {
        return;
    }

    const wxColour bg = ColorsFor(m_state.theme).background;
    m_renderer.BeginFrame(size.GetWidth(), size.GetHeight(),
                          { bg.Red() / 255.0f, bg.Green() / 255.0f,
                            bg.Blue() / 255.0f });

    const wxColour grid = ColorsFor(m_state.theme).grid;
    const WaveColor gridColor{ grid.Red() / 255.0f, grid.Green() / 255.0f,
                               grid.Blue() / 255.0f };
    for (const WaveformTick& tick : frame.ticks) {
        const float x = WaveformTimeToX(m_state, size.GetWidth(), tick.time);
        m_renderer.DrawGridLine(x, static_cast<float>(m_state.headerHeight),
                                x, static_cast<float>(size.GetHeight()), gridColor);
    }

    int row = 0;
    for (const WaveformSignalData& signal : frame.signals) {
        const WaveColor& color = WavePalette(row);
        m_renderer.DrawPolyline(signal.polyline, color);
        ++row;
    }

    // ── W3 覆盖层：A-B / 播放头 / Marker / 事件 ──
    if (m_state.hasAB) {
        const WaveColor abColor{ 251.0f / 255.0f, 146.0f / 255.0f, 60.0f / 255.0f };
        const float xa = WaveformTimeToX(m_state, size.GetWidth(), m_state.abA);
        const float xb = WaveformTimeToX(m_state, size.GetWidth(), m_state.abB);
        m_renderer.DrawGridLine(xa, static_cast<float>(m_state.headerHeight),
                                xa, static_cast<float>(size.GetHeight()), abColor);
        m_renderer.DrawGridLine(xb, static_cast<float>(m_state.headerHeight),
                                xb, static_cast<float>(size.GetHeight()), abColor);
    }
    if (m_state.hasPlayhead) {
        const WaveColor playheadColor{ 226.0f / 255.0f, 232.0f / 255.0f, 240.0f / 255.0f };
        const float xp = WaveformTimeToX(m_state, size.GetWidth(), m_state.playhead);
        m_renderer.DrawGridLine(xp, static_cast<float>(m_state.headerHeight),
                                xp, static_cast<float>(size.GetHeight()), playheadColor);
    }
    for (const WaveMarker& marker : m_state.markers) {
        const WaveColor color{ ((marker.color >> 16) & 0xFF) / 255.0f,
                               ((marker.color >> 8) & 0xFF) / 255.0f,
                               (marker.color & 0xFF) / 255.0f };
        const float x = WaveformTimeToX(m_state, size.GetWidth(), marker.time);
        m_renderer.DrawGridLine(x, static_cast<float>(m_state.headerHeight),
                                x, static_cast<float>(size.GetHeight()), color);
    }
    for (const WaveEvent& event : m_state.events) {
        const WaveColor color{ ((event.color >> 16) & 0xFF) / 255.0f,
                               ((event.color >> 8) & 0xFF) / 255.0f,
                               (event.color & 0xFF) / 255.0f };
        const float x = WaveformTimeToX(m_state, size.GetWidth(), event.time);
        m_renderer.DrawGridLine(x, static_cast<float>(m_state.headerHeight),
                                x, static_cast<float>(size.GetHeight()), color);
    }

    wxBitmap textBitmap;
    if (BuildTextLayerBitmap(this, frame, m_state, size.GetWidth(), size.GetHeight(),
                             textBitmap) &&
        textBitmap.IsOk()) {
        std::vector<std::uint8_t> rgba;
        BitmapToRgba(textBitmap, size.GetWidth(), size.GetHeight(), rgba);
        m_renderer.UploadTextOverlay(size.GetWidth(), size.GetHeight(), rgba.data());
        m_renderer.DrawTextOverlay();
    }

    m_renderer.EndFrame();
    SwapBuffers();
}

void WaveformGLCanvas::OnMouseWheel(wxMouseEvent& event)
{
    const wxSize size = GetClientSize();
    const int plotWidth = size.GetWidth() - m_state.leftMargin - m_state.rightMargin;
    const double frac = plotWidth > 0
        ? static_cast<double>(event.GetX() - m_state.leftMargin) / plotWidth
        : 0.5;
    WaveViewInteraction::ZoomAt(m_state, frac, event.GetWheelRotation() > 0 ? 0.7 : 1.45);
    Refresh();
    if (m_onViewChanged) m_onViewChanged();
}

void WaveformGLCanvas::OnMouseDown(wxMouseEvent& event)
{
    if (event.RightDown()) {
        m_rightDragging = true;
        m_rightStartX = event.GetX();
        m_lastMouseX = event.GetX();
        CaptureMouse();
        return;
    }
    m_dragging = true;
    m_lastMouseX = event.GetX();
    m_dragStartX = event.GetX();
    m_ctrlDrag = event.ControlDown();
    SetFocus();
}

void WaveformGLCanvas::OnMouseMove(wxMouseEvent& event)
{
    if (m_rightDragging && event.RightIsDown()) {
        m_lastMouseX = event.GetX();
        return;
    }
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
        Refresh();
        return;
    }
    const double frac = static_cast<double>(event.GetX() - m_lastMouseX) / plotWidth;
    WaveViewInteraction::PanBy(m_state, -frac);
    m_lastMouseX = event.GetX();
    Refresh();
}

void WaveformGLCanvas::OnMouseUp(wxMouseEvent& event)
{
    if (event.RightUp() && m_rightDragging) {
        m_rightDragging = false;
        if (HasCapture()) ReleaseMouse();
        const wxPoint point(event.GetX(), event.GetY());
        const auto a = WaveViewInteraction::TimeAtX(m_state, GetClientSize().GetWidth(),
                                                     m_rightStartX);
        const auto b = WaveViewInteraction::TimeAtX(m_state, GetClientSize().GetWidth(),
                                                     event.GetX());
        if (m_onContextMenu) m_onContextMenu(point, a, b);
        return;
    }
    const wxSize size = GetClientSize();
    const bool wasCtrl = m_ctrlDrag;
    const bool wasDrag = m_dragging;
    const bool moved = std::abs(m_lastMouseX - m_dragStartX) >= 3;
    m_dragging = false;
    m_ctrlDrag = false;
    if (!wasDrag) return;
    if (wasCtrl) {
        if (!moved) {
            m_state.hasAB = false;
        }
        Refresh();
        if (m_onViewChanged) m_onViewChanged();
        return;
    }
    if (moved) {
        if (m_onViewChanged) m_onViewChanged();
        return;
    }
    const sigflow::trace::TimeValue t =
        WaveViewInteraction::TimeAtX(m_state, size.GetWidth(), m_dragStartX);
    if (wxGetKeyState(WXK_SHIFT)) {
        WaveMarker marker;
        marker.time = t;
        marker.label = "M" + std::to_string(m_state.markers.size() + 1);
        m_state.markers.push_back(marker);
    } else {
        m_state.playhead = t;
        m_state.hasPlayhead = true;
    }
    Refresh();
    if (m_onViewChanged) m_onViewChanged();
}

void WaveformGLCanvas::OnKeyDown(wxKeyEvent& event)
{
    const int key = event.GetKeyCode();
    if (key == '+' || key == WXK_ADD || key == '=') {
        WaveViewInteraction::ZoomStep(m_state, true);
    } else if (key == '-' || key == WXK_SUBTRACT) {
        WaveViewInteraction::ZoomStep(m_state, false);
    } else if (key == '0') {
        WaveViewInteraction::Reset(m_state);
    } else if (key == WXK_LEFT) {
        WaveViewInteraction::PanBy(m_state, -0.1);
    } else if (key == WXK_RIGHT) {
        WaveViewInteraction::PanBy(m_state, 0.1);
    } else if (key == WXK_HOME) {
        WaveViewInteraction::JumpToTime(m_state, 0);
    } else if (key == WXK_END) {
        WaveViewInteraction::JumpToTime(m_state, m_state.maxTime);
    } else {
        event.Skip();
        return;
    }
    Refresh();
    if (m_onViewChanged) m_onViewChanged();
}

} // namespace wave
} // namespace sigflow
