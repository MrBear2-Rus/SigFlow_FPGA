#pragma once

#include "WaveViewState.h"
#include "../trace/TraceSource.h"

#include "ui/UiTheme.h"   // T-140：主题订阅句柄（析构自动解绑，THE-028）

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

    // T-140 / THE-030：重取 ColorsForThemed 的 miniMapBackground 并 Refresh。
    // 只重取色，不改 m_state（时间窗/可见信号/观察档）与任何波形语义。
    void ReapplyTheme();

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
    UiTheme::Subscription m_themeSubscription;
};

} // namespace wave
} // namespace sigflow
