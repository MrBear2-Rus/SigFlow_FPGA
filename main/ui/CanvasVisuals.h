#pragma once

#include "ui/UiTheme.h"
#include "Wire.h" // CanvasDraw 的统一画布尺寸常量
#include <wx/graphics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// 呈现和选中标记热区 helper；不修改模型边界或工具状态。units 是每个屏幕 DIP 的画布单位。
namespace CanvasVisuals {
inline double ScreenDip(const wxWindow* window, double zoom)
{
    // 世界坐标为 DIP，GC 已包含显示器 DPI；只需抵消用户 zoom。
    return 1.0 / std::max(zoom, 0.01);
}

inline wxFont WorldFont(const wxWindow* window, UiTheme::TextSize size,
                        bool mono = false, UiTheme::Weight weight = UiTheme::Weight::Normal)
{
    wxFont font = mono ? UiTheme::MonoFont(window, size, weight) : UiTheme::UiFont(window, size, weight);
    font.SetPixelSize(wxSize(0, static_cast<int>(size))); // GC 负责 DIP→设备像素，避免双重 DPI。
    return font;
}

inline wxRect PinCardBounds(const wxPoint& anchor, bool input)
{
    const int width = input ? 54 : 56;
    return wxRect(input ? anchor.x-width : anchor.x, anchor.y-15, width, 30);
}

inline void DrawPinCard(wxGraphicsContext& gc, const wxPoint& anchor, bool input,
                       const wxString& label, const wxWindow* window, double units)
{
    const auto r = PinCardBounds(anchor,input);
    const auto& c = UiTheme::Palette();
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.stroke).Width(units)));
    gc.SetBrush(wxBrush(c.surfaceStrong));
    gc.DrawRoundedRectangle(r.x,r.y,r.width,r.height,8);
    gc.SetFont(WorldFont(window,UiTheme::TextSize::P13,true),c.text1);
    double w=0,h=0; gc.GetTextExtent(label,&w,&h);
    const auto matrix=gc.GetTransform();
    const double fit=std::min(1.0,(r.width-8)/std::max(w,1.0));
    gc.Translate(r.x+r.width/2.0,r.y+r.height/2.0);gc.Scale(fit,fit);
    gc.DrawText(label,-w/2,-h/2);gc.SetTransform(matrix);
}

inline wxRect2DDouble SelectionBounds(const wxRect& bounds, double units)
{
    return {bounds.x - 2 * units, bounds.y - 2 * units,
            bounds.width + 4 * units, bounds.height + 4 * units};
}

inline std::array<wxPoint2DDouble, 8> HandleCenters(const wxRect2DDouble& r)
{
    const double x = r.m_x, y = r.m_y;
    const double right = r.GetRight(), bottom = r.GetBottom();
    const double cx = (x + right) / 2, cy = (y + bottom) / 2;
    return {{{x,y}, {cx,y}, {right,y}, {x,cy}, {right,cy},
             {x,bottom}, {cx,bottom}, {right,bottom}}};
}

inline bool HitHandle(const wxRect& bounds, const wxPoint& world, double units)
{
    const double radius=11*units;
    for (const auto& p:HandleCenters(SelectionBounds(bounds,units))) {
        if (std::hypot(world.x-p.m_x,world.y-p.m_y)<=radius) return true;
    }
    return false;
}

inline void DrawSelection(wxGraphicsContext& gc, const wxRect& bounds, double units, bool outline = true)
{
    const auto r = SelectionBounds(bounds, units);
    const auto& c = UiTheme::Palette();
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.accent).Width(CanvasDraw::kSelectionWidth * units)));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    const double radius = std::min({12 * units, r.m_width / 2, r.m_height / 2});
    // 元件只显示八个选中标记；文本等对象按需显示包围框。
    if (outline) gc.DrawRoundedRectangle(r.m_x, r.m_y, r.m_width, r.m_height, radius);
    // 与 app.html 的 SVG 一样，八个标记只描述选中包围盒，不伪造缩放交互。
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(c.accent));
    for (const auto& p : HandleCenters(r)) {
        gc.DrawRoundedRectangle(p.m_x - 3.5 * units, p.m_y - 3.5 * units,
                                7 * units, 7 * units, 1.5 * units);
    }
}

// GDI+ 的 user dash 长度是笔宽倍数，wxDash 又只支持整数。
// 直接按路径弧长切分，才能在 1.6 DIP 笔宽下保持 6 DIP / 5 DIP，而非 9.6 / 8。
inline void DrawGhost(wxGraphicsContext& gc, const wxRect& bounds,
                      const wxString& label, const wxFont& font, double units)
{
    if (bounds.width <= 0 || bounds.height <= 0) return;
    const double x = bounds.x, y = bounds.y;
    const double right = x + bounds.width, bottom = y + bounds.height;
    const double radius = std::min({10 * units, bounds.width / 2.0, bounds.height / 2.0});
    const std::array<wxPoint2DDouble, 4> corners = {{{right-radius,y+radius},
        {right-radius,bottom-radius}, {x+radius,bottom-radius}, {x+radius,y+radius}}};
    std::vector<wxPoint2DDouble> points;
    points.reserve(37);
    constexpr double pi = 3.14159265358979323846;
    for (int corner = 0; corner < 4; ++corner) {
        for (int step = 0; step <= 8; ++step) {
            const double a = (-90 + corner * 90 + step * 90.0 / 8) * pi / 180;
            points.emplace_back(corners[corner].m_x + radius * std::cos(a),
                                corners[corner].m_y + radius * std::sin(a));
        }
    }
    points.push_back(points.front());
    auto path = gc.CreatePath();
    bool ink = true, connected = false;
    double remaining = 6 * units;
    for (size_t i = 1; i < points.size(); ++i) {
        auto p = points[i-1];
        const double dx = points[i].m_x - p.m_x, dy = points[i].m_y - p.m_y;
        const double length = std::hypot(dx, dy);
        if (length < 1e-9) continue;
        double travelled = 0;
        while (travelled < length - 1e-9) {
            const double step = std::min(remaining, length - travelled);
            const wxPoint2DDouble end(p.m_x + dx / length * step, p.m_y + dy / length * step);
            if (ink) {
                if (!connected) path.MoveToPoint(p.m_x, p.m_y);
                path.AddLineToPoint(end.m_x, end.m_y);
                connected = true;
            }
            p = end; travelled += step; remaining -= step;
            if (remaining < 1e-9) {
                ink = !ink; remaining = (ink ? 6 : 5) * units;
                if (!ink) connected = false;
            }
        }
    }
    const auto& c = UiTheme::Palette();
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.brand).Width(CanvasDraw::kSelectionWidth * units).Cap(wxCAP_BUTT)));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.StrokePath(path);
    gc.SetFont(font, c.brand);
    // 字体按 canvas zoom 缩放；将过长名称缩至框内，不改变元件几何。
    double w = 0, h = 0;
    gc.GetTextExtent(label, &w, &h);
    const double fit = std::min(1.0, std::max(1.0, bounds.width - 8 * units) / std::max(w, 1.0));
    const auto matrix = gc.GetTransform();
    gc.Translate(x + bounds.width / 2.0, y + bounds.height / 2.0);
    gc.Scale(fit, fit);
    gc.DrawText(label, -w / 2.0, -h / 2.0);
    gc.SetTransform(matrix);
}
} // namespace CanvasVisuals
