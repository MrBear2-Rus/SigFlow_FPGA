#pragma once

#include "WaveViewState.h"
#include "../trace/TraceSource.h"

#include <functional>
#include <memory>

#include <wx/panel.h>

namespace sigflow {
namespace wave {

class WaveformMiniMap : public wxPanel {
public:
    WaveformMiniMap(wxWindow* parent, WaveViewState& state,
                    std::shared_ptr<sigflow::trace::TraceSource>& source,
                    std::function<void()> onChanged);

    void RefreshOverview();

private:
    void OnPaint(wxPaintEvent& event);
    void OnMouseDown(wxMouseEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnMouseUp(wxMouseEvent& event);
    void MoveViewport(int x);
    sigflow::trace::TimeValue TimeAtX(int x) const;

    WaveViewState& m_state;
    std::shared_ptr<sigflow::trace::TraceSource>& m_source;
    std::function<void()> m_onChanged;
    bool m_dragging = false;
    int m_lastX = 0;
};

} // namespace wave
} // namespace sigflow
