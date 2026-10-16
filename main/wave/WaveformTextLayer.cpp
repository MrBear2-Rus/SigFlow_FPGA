#include "WaveformTextLayer.h"
#include "WaveTheme.h"

#include <wx/dcmemory.h>
#include <wx/font.h>

#include <algorithm>
#include <cstdio>

namespace sigflow {
namespace wave {

void DrawTextLayer(wxDC& dc, const WaveformFrame& frame,
                   const WaveViewState& state, int width, int height)
{
    // CMP-064：信号名 11 DIP 等宽；文字/语义色一律经 UiTheme token 取用（SAFE-012）
    const UiTheme::Colours& ui = UiTheme::PaletteFor(UiModeFor(state.theme));
    const WaveThemeColors colors = ColorsForThemed(state.theme);
    dc.SetFont(UiTheme::MonoFont(dc.GetWindow(), UiTheme::TextSize::P11));
    dc.SetBackgroundMode(wxTRANSPARENT);
    dc.SetTextForeground(colors.text);
    dc.DrawText(frame.timeUnitLabel, 4, 4);

    // 时间轴刻度
    for (const WaveformTick& tick : frame.ticks) {
        const int x = static_cast<int>(WaveformTimeToX(state, width, tick.time));
        if (x < state.leftMargin || x > width - state.rightMargin) continue;
        dc.DrawText(tick.label, x - 12, 4);
    }

    // 信号名（leftMargin 区域内右对齐）+ 向量值标注
    int row = 0;
    for (const WaveformSignalData& signal : frame.signals) {
        const int rowTop = state.headerHeight + row * state.rowHeight;
        const wxSize extent = dc.GetTextExtent(signal.name);
        dc.SetTextForeground(ui.text2);
        dc.DrawText(signal.name,
                    std::max(4, state.leftMargin - 12 - extent.GetWidth()),
                    rowTop + std::max(0, (state.rowHeight - extent.GetHeight()) / 2));
        if (signal.vector) {
            dc.SetTextForeground(ui.text3);
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
        dc.SetTextForeground(ui.warn);
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
        dc.SetTextForeground(ui.text1);
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

}

bool BuildTextLayerBitmap(wxWindow* win, const WaveformFrame& frame,
                          const WaveViewState& state, int width, int height,
                          wxBitmap& outBitmap)
{
    if (width <= 0 || height <= 0) return false;
    outBitmap = wxBitmap(width, height, 32);
    if (!outBitmap.IsOk()) return false;

    wxMemoryDC dc(outBitmap);
    dc.SetBackground(wxBrush(wxColour(0, 0, 0, 0)));
    dc.Clear();
    DrawTextLayer(dc, frame, state, width, height);
    dc.SelectObject(wxNullBitmap);
    return true;
}

} // namespace wave
} // namespace sigflow
