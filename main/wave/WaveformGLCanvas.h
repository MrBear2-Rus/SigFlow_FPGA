#pragma once

#include "../trace/TraceSource.h"
#include "WaveViewState.h"
#include "WaveformGLRenderer.h"

#include <functional>
#include <memory>

#include <wx/glcanvas.h>

namespace sigflow {
namespace wave {

// OpenGL 后端画布（T-W2-01/02）。
class WaveformGLCanvas : public wxGLCanvas {
public:
    WaveformGLCanvas(wxWindow* parent, WaveViewState& state,
                     std::shared_ptr<sigflow::trace::TraceSource>& source,
                     std::function<void()> onGlFailed,
                     std::function<void()> onViewChanged,
                     std::function<void(const wxPoint&, sigflow::trace::TimeValue,
                                        sigflow::trace::TimeValue)> onContextMenu);
    ~WaveformGLCanvas() override;

    bool Ready() const { return m_glReady; }

private:
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    void OnMouseDown(wxMouseEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnMouseUp(wxMouseEvent& event);
    void OnKeyDown(wxKeyEvent& event);

    bool EnsureContext();
    void RenderFrame();
    void NotifyFailure();

    WaveViewState& m_state;
    std::shared_ptr<sigflow::trace::TraceSource>& m_source;
    std::function<void()> m_onGlFailed;
    std::function<void()> m_onViewChanged;
    std::function<void(const wxPoint&, sigflow::trace::TimeValue,
                       sigflow::trace::TimeValue)> m_onContextMenu;
    wxGLContext* m_glContext = nullptr;
    WaveformGLRenderer m_renderer;
    bool m_glReady = false;
    bool m_dragging = false;
    bool m_ctrlDrag = false;
    bool m_rightDragging = false;
    int m_lastMouseX = 0;
    int m_dragStartX = 0;
    int m_rightStartX = 0;
};

} // namespace wave
} // namespace sigflow
