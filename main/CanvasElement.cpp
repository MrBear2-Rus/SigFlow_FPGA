#ifdef Polygon
#undef Polygon
#endif

#include "CanvasElement.h"
#include <variant>
#include <cmath>
#include <algorithm>
#include <limits>
#include <wx/dcgraph.h>
#include <wx/graphics.h>
#include <sstream>

std::vector<wxPoint> CanvasElement::CalculateBezier(const Point& p0, const Point& p1, const Point& p2, int segments) const
{
    std::vector<wxPoint> pts;
    for (int i = 0; i <= segments; ++i) {
        double t = double(i) / segments;
        double x = (1 - t) * (1 - t) * p0.x + 2 * (1 - t) * t * p1.x + t * t * p2.x;
        double y = (1 - t) * (1 - t) * p0.y + 2 * (1 - t) * t * p1.y + t * t * p2.y;
        pts.emplace_back(static_cast<int>(x), static_cast<int>(y));
    }
    return pts;
}

// 三次贝塞尔曲线计算（4个控制点）
std::vector<wxPoint> CanvasElement::CalculateCubicBezier(const Point& p0, const Point& p1, const Point& p2, const Point& p3, int segments) const
{
    std::vector<wxPoint> pts;
    for (int i = 0; i <= segments; ++i) {
        double t = double(i) / segments;
        double u = 1 - t;
        double x = u*u*u * p0.x + 3*u*u*t * p1.x + 3*u*t*t * p2.x + t*t*t * p3.x;
        double y = u*u*u * p0.y + 3*u*u*t * p1.y + 3*u*t*t * p2.y + t*t*t * p3.y;
        pts.emplace_back(static_cast<int>(x), static_cast<int>(y));
    }
    return pts;
}

CanvasElement::CanvasElement(const wxPoint& pos)
    : m_pos(pos)
{
}

void CanvasElement::Draw(wxGraphicsContext* gc) const
{
    if (!gc) return;

    wxGraphicsMatrix origMatrix = gc->GetTransform();

    // 2. 提取平移和缩放（和之前一样，只是用对象调用 Get()）
    double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
    double origTx = 0.0, origTy = 0.0;
    origMatrix.Get(&a, &b, &c, &d, &origTx, &origTy); // 对象调用 Get()
    double origSx = a;
    double origSy = d;
    gc->Translate(m_pos.x, m_pos.y);

    // -------------------------- 3. 绘制所有形状（逻辑不变，补充 Path 分支避免 visit 遗漏） --------------------------
    for (const auto& shape : m_shapes) {
        std::visit([&](auto&& s) {
            using T = std::decay_t<decltype(s)>;

            // 分支1：Line
            if constexpr (std::is_same_v<T, Line>) {
                gc->SetPen(wxPen(s.color, 3.0));
                gc->StrokeLine(s.start.x, s.start.y, s.end.x, s.end.y);
            }
            // 分支2：ArcShape
            else if constexpr (std::is_same_v<T, ArcShape>) {
                gc->SetPen(wxPen(s.color, 3.0));
                wxGraphicsPath path = gc->CreatePath();
                path.AddArc(s.center.x, s.center.y, s.radius,
                    s.startAngle * M_PI / 180, s.endAngle * M_PI / 180, true);
                gc->StrokePath(path);
            }
            // 分支3：Text
            else if constexpr (std::is_same_v<T, Text>) {
                wxFont font(s.fontSize, wxFONTFAMILY_DEFAULT,
                    wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
                gc->SetFont(font, s.color);
                gc->DrawText(s.text, s.pos.x, s.pos.y);
            }
            // 分支4：Circle
            else if constexpr (std::is_same_v<T, Circle>) {
                gc->SetPen(wxPen(s.color, 3.0));
                gc->SetBrush(s.fill ? wxBrush(s.fillColor) : *wxTRANSPARENT_BRUSH);
                gc->DrawEllipse(s.center.x - s.radius, s.center.y - s.radius,
                    s.radius * 2, s.radius * 2);
            }
            // 分支5：PolyShape
            else if constexpr (std::is_same_v<T, PolyShape>) {
                wxGraphicsPath path = gc->CreatePath();
                if (!s.pts.empty()) {
                    path.MoveToPoint(s.pts[0].x, s.pts[0].y);
                    for (size_t i = 1; i < s.pts.size(); ++i) {
                        path.AddLineToPoint(s.pts[i].x, s.pts[i].y);
                    }
                    path.CloseSubpath();
                }
                gc->SetPen(wxPen(s.color, 3.0));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->StrokePath(path);
            }
            // 分支6：BezierShape
            else if constexpr (std::is_same_v<T, BezierShape>) {
                gc->SetPen(wxPen(s.color, 3.0));
                wxGraphicsPath path = gc->CreatePath();
                path.MoveToPoint(s.p0.x, s.p0.y);
                path.AddCurveToPoint(s.p1.x, s.p1.y, s.p1.x, s.p1.y, s.p2.x, s.p2.y);
                gc->StrokePath(path);
            }
            // 分支7：CubicBezierShape（三次贝塞尔曲线）
            else if constexpr (std::is_same_v<T, CubicBezierShape>) {
                gc->SetPen(wxPen(s.color, 3.0));
                wxGraphicsPath path = gc->CreatePath();
                path.MoveToPoint(s.p0.x, s.p0.y);
                path.AddCurveToPoint(s.p1.x, s.p1.y, s.p2.x, s.p2.y, s.p3.x, s.p3.y);
                gc->StrokePath(path);
            }
            // 分支8：Path（补充完整，避免覆盖不全）
            else if constexpr (std::is_same_v<T, Path>) {
                gc->SetPen(wxPen(s.stroke, s.strokeWidth));
                gc->SetBrush(s.fill ? wxBrush(s.stroke) : *wxTRANSPARENT_BRUSH);

                wxGraphicsPath gPath = gc->CreatePath();

                // 如果你的 d 字符串符合 SVG 标准，且你不想引入复杂的解析器
                // 这里演示如何从字符串构建路径（假设格式为简单指令）
                // 如果你有现成的解析函数，请替换此处逻辑
                std::stringstream ss(s.d);
                char cmd;
                double x, y;
                while (ss >> cmd >> x >> y) {
                    if (cmd == 'M' || cmd == 'm') gPath.MoveToPoint(x, y);
                    else if (cmd == 'L' || cmd == 'l') gPath.AddLineToPoint(x, y);
                }

                if (s.fill) gPath.CloseSubpath();
                gc->DrawPath(gPath);
            }

            }, shape);  // 确保 std::visit 的 lambda 正确闭合
    }

    // 绘制输入引脚（蓝色圆点）
    for (const auto& pin : m_inputPins) {
        gc->SetPen(wxPen(wxColour(0, 0, 255), 1.0)); // 蓝色边框
        gc->SetBrush(wxBrush(wxColour(0, 0, 255)));  // 蓝色填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    // 绘制输出引脚（红色圆点）
    for (const auto& pin : m_outputPins) {
        gc->SetPen(wxPen(wxColour(255, 0, 0), 1.0)); // 红色边框
        gc->SetBrush(wxBrush(wxColour(255, 0, 0)));  // 红色填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    // -------------------------- 4. 恢复原始上下文变换 --------------------------
    gc->SetTransform(origMatrix); // 这里不用 *，直接传对象
}

wxRect CanvasElement::GetBounds() const
{
    // GetBounds 方法保持不变
    if (m_shapes.empty()) return wxRect(m_pos, wxSize(1, 1));

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxY = std::numeric_limits<int>::min();

    auto update = [&](const Point& p) {
        int x = m_pos.x + p.x;
        int y = m_pos.y + p.y;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        };

    for (const auto& shape : m_shapes) {
        auto visitor = [&](const auto& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, PolyShape>) {
                for (const auto& pt : arg.pts) update(pt);
            }
            else if constexpr (std::is_same_v<T, Line>) {
                update(arg.start);
                update(arg.end);
            }
            else if constexpr (std::is_same_v<T, Circle>) {
                update(Point(arg.center.x - arg.radius, arg.center.y - arg.radius));
                update(Point(arg.center.x + arg.radius, arg.center.y + arg.radius));
            }
            else if constexpr (std::is_same_v<T, Text>) {
                update(arg.pos);
                update(Point(arg.pos.x + 20, arg.pos.y + 10));
            }
            else if constexpr (std::is_same_v<T, ArcShape>) {
                update(Point(arg.center.x - arg.radius, arg.center.y - arg.radius));
                update(Point(arg.center.x + arg.radius, arg.center.y + arg.radius));
            }
            else if constexpr (std::is_same_v<T, BezierShape>) {
                auto pts = CalculateBezier(arg.p0, arg.p1, arg.p2, 32);
                for (const auto& wp : pts) {
                    Point p{ wp.x, wp.y };
                    update(p);
                }
            }
            else if constexpr (std::is_same_v<T, CubicBezierShape>) {
                auto pts = CalculateCubicBezier(arg.p0, arg.p1, arg.p2, arg.p3, 32);
                for (const auto& wp : pts) {
                    Point p{ wp.x, wp.y };
                    update(p);
                }
            }
            else if constexpr (std::is_same_v<T, Path>) {
                if (arg.d.find("A 16 28") != std::string::npos) {
                    update(Point(10, 12));
                    update(Point(50, 52));
                }
            }
            };

        std::visit(visitor, shape);
    }

    for (const auto& pin : m_inputPins) {
        update(pin.pos);
        update(Point(pin.pos.x - 3, pin.pos.y - 3)); // 更新为圆点的边界
        update(Point(pin.pos.x + 3, pin.pos.y + 3));
    }

    for (const auto& pin : m_outputPins) {
        update(pin.pos);
        update(Point(pin.pos.x - 3, pin.pos.y - 3)); // 更新为圆点的边界
        update(Point(pin.pos.x + 3, pin.pos.y + 3));
    }

    const int grid = 20;

    // 1. 对齐左上角：向外扩充，确保包含所有内容
    int alignedMinX = (minX / grid) * grid;
    if (minX < 0 && minX % grid != 0) alignedMinX -= grid; // 处理负数情况

    int alignedMinY = (minY / grid) * grid;
    if (minY < 0 && minY % grid != 0) alignedMinY -= grid;

    // 2. 对齐右下角：向上取整到最近的 20 倍数
    int alignedMaxX = ((maxX + (grid - 1)) / grid) * grid;
    int alignedMaxY = ((maxY + (grid - 1)) / grid) * grid;

    // 3. 返回对齐后的矩形
    return wxRect(alignedMinX, alignedMinY,
        alignedMaxX - alignedMinX,
        alignedMaxY - alignedMinY);
}


void InputElement::Draw(wxGraphicsContext * gc) const
{
    if (!gc) return;

    wxGraphicsMatrix origMatrix = gc->GetTransform();

    // 2. 提取平移和缩放（和之前一样，只是用对象调用 Get()）
    double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
    double origTx = 0.0, origTy = 0.0;
    origMatrix.Get(&a, &b, &c, &d, &origTx, &origTy); // 对象调用 Get()
    double origSx = a;
    double origSy = d;
    gc->Translate(m_pos.x, m_pos.y);

    // -------------------------- 3. 绘制所有形状（逻辑不变，补充 Path 分支避免 visit 遗漏） --------------------------
    for (const auto& shape : m_shapes) {
        std::visit([&](auto&& s) {
            using T = std::decay_t<decltype(s)>;

            // 对于Pin_Input元件，根据状态修改绘制
            if (m_id == "Pin_Input") {
                if constexpr (std::is_same_v<T, Circle>) {
                    // 根据状态选择颜色
                    wxColour circleColor = m_state ? wxColour(0, 255, 0) : wxColour(0, 128, 0); // 深绿色 : 绿色
                    wxColour fillColor = m_state ? wxColour(0, 255, 0) : wxColour(0, 128, 0);

                    gc->SetPen(wxPen(circleColor, 3.0));
                    gc->SetBrush(wxBrush(fillColor));
                    gc->DrawEllipse(s.center.x - s.radius, s.center.y - s.radius,
                        s.radius * 2, s.radius * 2);
                    return; // 跳过原始绘制
                }
                else if constexpr (std::is_same_v<T, Text>) {
                    // 根据状态显示不同的文本
                    wxString displayText = m_state ? "1" : "0";
                    wxFont font(s.fontSize, wxFONTFAMILY_DEFAULT,
                        wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
                    gc->SetFont(font, *wxWHITE); // 保持白色文本
                    gc->DrawText(displayText, s.pos.x, s.pos.y);
                    return; // 跳过原始绘制
                }
            }
            }, shape);  // 确保 std::visit 的 lambda 正确闭合
    }

    // 绘制输入引脚（蓝色圆点）
    for (const auto& pin : m_inputPins) {
        gc->SetPen(wxPen(wxColour(0, 0, 255), 1.0)); // 蓝色边框
        gc->SetBrush(wxBrush(wxColour(0, 0, 255)));  // 蓝色填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    // 绘制输出引脚（红色圆点）
    for (const auto& pin : m_outputPins) {
        gc->SetPen(wxPen(wxColour(255, 0, 0), 1.0)); // 红色边框
        gc->SetBrush(wxBrush(wxColour(255, 0, 0)));  // 红色填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    // -------------------------- 4. 恢复原始上下文变换 --------------------------
    gc->SetTransform(origMatrix); // 这里不用 *，直接传对象
}

