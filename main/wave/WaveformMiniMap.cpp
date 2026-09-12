#include "WaveformMiniMap.h"

#include "WaveformRenderData.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/pen.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace sigflow {
namespace wave {
namespace {

sigflow::trace::TimeValue FullSpan(const WaveViewState& state)
{
    return state.maxTime == std::numeric_limits<sigflow::trace::TimeValue>::max()
        ? state.maxTime : state.maxTime + 1;
}

int ClampX(int x, int width)
{
    return std::max(0, std::min(width, x));
}

} // namespace

WaveformMiniMap::WaveformMiniMap(
    wxWindow* parent, WaveViewState& state,
    std::shared_ptr<sigflow::trace::TraceSource>& source,
    std::function<void()> onChanged)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      m_state(state), m_source(source), m_onChanged(std::move(onChanged))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(ColorsFor(m_state.theme).miniMapBackground);
    Bind(wxEVT_PAINT, &WaveformMiniMap::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &WaveformMiniMap::OnMouseDown, this);
    Bind(wxEVT_MOTION, &WaveformMiniMap::OnMouseMove, this);
    Bind(wxEVT_LEFT_UP, &WaveformMiniMap::OnMouseUp, this);
}

void WaveformMiniMap::RefreshOverview()
{
    Refresh(false);
}

sigflow::trace::TimeValue WaveformMiniMap::TimeAtX(int x) const
{
    const int width = std::max(1, GetClientSize().GetWidth());
    const auto fullSpan = FullSpan(m_state);
    return static_cast<sigflow::trace::TimeValue>(
        static_cast<double>(ClampX(x, width)) / width * fullSpan);
}

void WaveformMiniMap::MoveViewport(int x)
{
    const auto fullSpan = FullSpan(m_state);
    const auto center = TimeAtX(x);
    const auto half = m_state.timeSpan / 2;
    m_state.timeOffset = center > half ? center - half : 0;
    m_state.Clamp();
    RefreshOverview();
    if (m_onChanged) m_onChanged();
}

void WaveformMiniMap::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    const wxSize size = GetClientSize();
    const WaveThemeColors colors = ColorsFor(m_state.theme);
    dc.SetBackground(wxBrush(colors.miniMapBackground));
    dc.Clear();
    if (size.x <= 0 || size.y <= 0) return;

    const auto fullSpan = FullSpan(m_state);
    if (fullSpan == 0) return;
    if (m_source) {
        int row = 0;
        for (const int id : m_state.visibleSignalIds) {
            const auto* signal = m_source->SignalById(id);
            if (!signal) continue;
            std::vector<sigflow::trace::Transition> transitions;
            std::string error;
            if (!m_source->Query(*signal, 0, m_state.maxTime, transitions, error)) continue;
            std::string current;
            if (!m_source->ValueAt(*signal, 0, current, error)) continue;
            const int y = 8 + (row % std::max(1, size.y - 16)) * 2;
            dc.SetPen(wxPen(wxColour(
                static_cast<unsigned char>(WavePalette(row).r * 255.0f),
                static_cast<unsigned char>(WavePalette(row).g * 255.0f),
                static_cast<unsigned char>(WavePalette(row).b * 255.0f))));
            int previousX = 0;
            int previousY = y;
            for (const auto& transition : transitions) {
                const int x = static_cast<int>(
                    static_cast<double>(transition.time) / fullSpan * size.x);
                const int nextY = (transition.value == "1") ? y - 2 : y + 2;
                dc.DrawLine(previousX, previousY, x, previousY);
                dc.DrawLine(x, previousY, x, nextY);
                previousX = x;
                previousY = nextY;
                current = transition.value;
            }
            dc.DrawLine(previousX, previousY, size.x, previousY);
            ++row;
        }
    }

    const int left = static_cast<int>(
        static_cast<double>(m_state.timeOffset) / fullSpan * size.x);
    const auto end = std::min(fullSpan, m_state.timeOffset + m_state.timeSpan);
    const int right = static_cast<int>(static_cast<double>(end) / fullSpan * size.x);
    dc.SetBrush(wxBrush(colors.viewport));
    dc.SetPen(wxPen(wxColour(96, 165, 250), 2));
    dc.DrawRectangle(left, 1, std::max(3, right - left), std::max(1, size.y - 2));
    dc.SetPen(wxPen(wxColour(100, 116, 139)));
    dc.DrawLine(0, size.y - 1, size.x, size.y - 1);
}

void WaveformMiniMap::OnMouseDown(wxMouseEvent& event)
{
    m_dragging = true;
    m_lastX = event.GetX();
    CaptureMouse();
    MoveViewport(m_lastX);
}

void WaveformMiniMap::OnMouseMove(wxMouseEvent& event)
{
    if (!m_dragging || !event.LeftIsDown()) return;
    if (event.GetX() != m_lastX) {
        MoveViewport(event.GetX());
        m_lastX = event.GetX();
    }
}

void WaveformMiniMap::OnMouseUp(wxMouseEvent&)
{
    m_dragging = false;
    if (HasCapture()) ReleaseMouse();
}

} // namespace wave
} // namespace sigflow
