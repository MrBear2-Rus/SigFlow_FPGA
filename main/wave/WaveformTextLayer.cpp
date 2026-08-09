#include "WaveformTextLayer.h"

#include <wx/dcmemory.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/font.h>

#include <algorithm>
#include <cstdio>

namespace sigflow {
namespace wave {

bool BuildTextLayerBitmap(wxWindow* win, const WaveformFrame& frame,
                          const WaveViewState& state, int width, int height,
                          wxBitmap& outBitmap)
{
    {
        wxFile file(wxFileName::GetTempDir() + "\\waveview_paint_trace.txt", wxFile::write_append);
        if (file.IsOpened()) file.Write("text enter\n");
    }
    if (width <= 0 || height <= 0) return false;
    outBitmap = wxBitmap(width, height, 32);
    if (!outBitmap.IsOk()) return false;
    {
        wxFile file(wxFileName::GetTempDir() + "\\waveview_paint_trace.txt", wxFile::write_append);
        if (file.IsOpened()) file.Write("text bitmap ok\n");
    }

    wxMemoryDC dc(outBitmap);
    dc.SetBackground(wxBrush(wxColour(0, 0, 0, 0)));
    dc.Clear();

    const wxFont font(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    dc.SetFont(font);
    dc.SetTextForeground(wxColour(203, 213, 225));

    // 时间轴刻度
    for (const WaveformTick& tick : frame.ticks) {
        const int x = static_cast<int>(WaveformTimeToX(state, width, tick.time));
        if (x < state.leftMargin || x > width - state.rightMargin) continue;
        dc.DrawText(tick.label, x - 12, 4);
    }

    // 信号名 + 向量值标注
    int row = 0;
    for (const WaveformSignalData& signal : frame.signals) {
        const int rowTop = state.headerHeight + row * state.rowHeight;
        dc.DrawText(signal.name, 8, rowTop + 7);
        if (signal.vector) {
            int drawn = 0;
            for (const auto& label : signal.valueLabels) {
                if (drawn >= 200) break;
                const int x = static_cast<int>(
                    WaveformTimeToX(state, width, label.first));
                if (x < state.leftMargin || x > width - state.rightMargin) continue;
                dc.DrawText(label.second, x + 2, rowTop + state.rowHeight - 10);
                ++drawn;
            }
        }
        ++row;
    }

    // ── W3 标签：A-B / 播放头 / Marker / 事件 ──
    if (state.hasAB) {
        dc.SetTextForeground(wxColour(251, 146, 60));
        const int xa = static_cast<int>(
            WaveformTimeToX(state, width, state.abA));
        const int xb = static_cast<int>(
            WaveformTimeToX(state, width, state.abB));
        if (xa >= state.leftMargin && xa <= width - state.rightMargin) {
            dc.DrawText("A", xa - 3, 14);
        }
        if (xb >= state.leftMargin && xb <= width - state.rightMargin) {
            dc.DrawText("B", xb - 3, 14);
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "ΔT=%llu",
                      static_cast<unsigned long long>(state.abB - state.abA));
        dc.DrawText(buffer, width - 130, 4);
    }
    if (state.hasPlayhead) {
        const int xp = static_cast<int>(
            WaveformTimeToX(state, width, state.playhead));
        dc.SetTextForeground(wxColour(226, 232, 240));
        if (xp >= state.leftMargin && xp <= width - state.rightMargin) {
            dc.DrawText("t=" + std::to_string(state.playhead), xp + 4, 22);
        }
    }
    for (const WaveMarker& marker : state.markers) {
        const int x = static_cast<int>(
            WaveformTimeToX(state, width, marker.time));
        if (x < state.leftMargin || x > width - state.rightMargin) continue;
        dc.SetTextForeground(wxColour((marker.color >> 16) & 0xFF,
                                      (marker.color >> 8) & 0xFF,
                                      marker.color & 0xFF));
        dc.DrawText(marker.label, x + 2, 14);
    }
    for (const WaveEvent& event : state.events) {
        const int x = static_cast<int>(
            WaveformTimeToX(state, width, event.time));
        if (x < state.leftMargin || x > width - state.rightMargin) continue;
        dc.SetTextForeground(wxColour((event.color >> 16) & 0xFF,
                                      (event.color >> 8) & 0xFF,
                                      event.color & 0xFF));
        dc.DrawText(event.label, x + 2, 28);
    }

    dc.SelectObject(wxNullBitmap);
    {
        wxFile file(wxFileName::GetTempDir() + "\\waveview_paint_trace.txt", wxFile::write_append);
        if (file.IsOpened()) file.Write("text exit\n");
    }
    return true;
}

} // namespace wave
} // namespace sigflow
