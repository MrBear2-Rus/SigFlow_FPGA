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
#include "SigTree.h"

std::vector<wxPoint> SecondElement::CalculateBezier(const Point& p0, const Point& p1, const Point& p2, int segments) const
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
std::vector<wxPoint> SecondElement::CalculateCubicBezier(const Point& p0, const Point& p1, const Point& p2, const Point& p3, int segments) const
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

SecondElement::SecondElement(SecondNode* sn, std::vector<SecondElement>& templates):self(sn) {
    switch (sn->secondType) {
    case SecondNodeType::GateInstance: {
        // 1. 在指针容器中查找
        GateInstNode* gn = static_cast<GateInstNode*>(sn);

        auto it = std::find_if(templates.begin(), templates.end(),
            [&](const SecondElement& e) {
                return e.type == SigFlowTree::ToString(gn->gatetype);
            });

        if (it != templates.end()) {
            // 2. 使用虚函数 Clone，确保获取的是真实的子类类型
            const SecondElement& templateElement = *it;

            this->type = templateElement.type;
            this->m_bound = templateElement.m_bound;
            this->m_shapes = templateElement.m_shapes;

            // 3. 深拷贝引脚，并修正引脚内部的指针指向当前 Node 的端口
            size_t inCount = std::min((size_t)sn->in_ports.size(), templateElement.m_inPins.size());
            for (size_t i = 0; i < inCount; i++) {
                Pin newPin = templateElement.m_inPins[i];
                newPin.SetSelf(&sn->in_ports[i]);
                m_inPins.push_back(newPin);
            }

            size_t outCount = std::min((size_t)sn->out_ports.size(), templateElement.m_outPins.size());
            for (size_t i = 0; i < outCount; i++) {
                Pin newPin = templateElement.m_outPins[i];
                newPin.SetSelf(&sn->out_ports[i]);
                m_outPins.push_back(newPin);
            }
        }
        else {
            this->type = "inst";
            MakeBlackBox();
        }
        break;
    }
    case SecondNodeType::ModuleInstance:{
        // 1. 在指针容器中查找
        ModuleInstNode* mn = static_cast<ModuleInstNode*>(sn);

        auto it = std::find_if(templates.begin(), templates.end(),
            [&](const SecondElement& e) {
                return e.type == mn->defIdentifier;
            });

        if (it != templates.end()) {
            // 2. 使用虚函数 Clone，确保获取的是真实的子类类型
            const SecondElement& templateElement = *it;

            this->type = templateElement.type;
            this->m_bound = templateElement.m_bound;
            this->m_shapes = templateElement.m_shapes;

            // 3. 深拷贝引脚，并修正引脚内部的指针指向当前 Node 的端口
            size_t inCount = std::min((size_t)sn->in_ports.size(), templateElement.m_inPins.size());
            for (size_t i = 0; i < inCount; i++) {
                Pin newPin = templateElement.m_inPins[i];
                newPin.SetSelf(&sn->in_ports[i]);
                m_inPins.push_back(newPin);
            }

            size_t outCount = std::min((size_t)sn->out_ports.size(), templateElement.m_outPins.size());
            for (size_t i = 0; i < outCount; i++) {
                Pin newPin = templateElement.m_outPins[i];
                newPin.SetSelf(&sn->out_ports[i]);
                m_outPins.push_back(newPin);
            }
        }
        else {
            this->type = "inst";
            MakeBlackBox();
        }
        break;
    }
    case SecondNodeType::ContinuousAssign:
    {
        this->type = "assign";
        MakeBlackBox();
        break;
    }
    case SecondNodeType::Always:
        
        {
        this->type = "always";
        MakeBlackBox();
        break;
    }


    }
}


void SecondElement::MakeBlackBox() {
    int in = self->in_ports.size();
    int out = self->out_ports.size();

    const int GRID = 20;
    const int BODY_WIDTH = 120;

    // 1. 计算引脚需要的最小垂直跨度
    auto getNeededHeight = [&](int count) {
        return (count <= 0) ? 0 : (count - 1) * GRID;
        };

    int heightIn = getNeededHeight(in);
    int heightOut = getNeededHeight(out);

    // 2. 确定主体高度（必须是 GRID 的倍数）
    int bodyHeight = std::max(heightIn, heightOut) + 2 * GRID;

    // 3. 计算 Pin 的起始 Y 坐标（相对于主体顶部 0 的偏移）
    // 为了垂直居中：(总高度 - 引脚占用高度) / 2
    auto getRelativeStartY = [&](int count) {
        if (count <= 0) return 0;
        int theoryTop = (bodyHeight - (count - 1) * GRID) / 2;
        return (theoryTop / GRID) * GRID; // 强制对齐网格
        };

    int inStartY = getRelativeStartY(in);
    int outStartY = getRelativeStartY(out);


    // 4. 绘制主体矩形：直接从 (0,0) 到 (BODY_WIDTH, bodyHeight)
    std::vector<Point> rect = {
        {0, 0}, {BODY_WIDTH, 0},
        {BODY_WIDTH, bodyHeight}, {0, bodyHeight}
    };
    m_shapes.push_back(PolyShape{ rect, wxColour(0,0,0) });

    // 5. 放置 Input Pins (位于左边线上 x = 0)
    for (int i = 0; i < in; ++i) {
        int y = inStartY + i * GRID;
        AddInputPin({ 0, y }, &self->in_ports[i]);
    }

    // 6. 放置 Output Pins (位于右边线上 x = BODY_WIDTH)
    for (int i = 0; i < out; ++i) {
        int y = outStartY + i * GRID;
        AddOutputPin({ BODY_WIDTH, y }, &self->out_ports[i]);
    }

    if (self->secondType == SecondNodeType::ModuleInstance) {
        ModuleInstNode* mn = static_cast<ModuleInstNode*>(self);
        for (int i = 0; i < mn->inout_ports.size(); ++i) {
            AddInOutputPin({ i * GRID,  bodyHeight }, &mn->inout_ports[i]);
        }
    }


    // 7. 设置边界：现在左上角是 (0,0)，宽高显而易见
    m_bound = wxRect(0, 0, BODY_WIDTH, bodyHeight);
}

wxString SecondElement::GetIdentifier() const { return self == nullptr ? wxString("none") : wxString(self->identifier); }

wxRect SecondElement::GetBounds() const {
    wxRect rect = m_bound;
    rect.SetPosition(m_pos);
    return rect;
}

wxRect TopModuleBox::GetBounds() const {
    wxRect rect = m_bound;
    rect.SetPosition(m_pos);
    return rect;
}

void SecondElement::Draw(wxGraphicsContext* gc) const
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
    for (const auto& pin : m_inPins) {
        gc->SetPen(wxPen(wxColour(0, 0, 255), 1.0)); // 蓝色边框
        gc->SetBrush(wxBrush(wxColour(0, 0, 255)));  // 蓝色填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    // 绘制输出引脚（红色圆点）
    for (const auto& pin : m_outPins) {
        gc->SetPen(wxPen(wxColour(255, 0, 0), 1.0)); // 红色边框
        gc->SetBrush(wxBrush(wxColour(255, 0, 0)));  // 红色填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    for (const auto& pin : m_inoutPins) {
        gc->SetPen(wxPen(wxColour(255, 255, 0), 1.0)); // 纯黄边框
        gc->SetBrush(wxBrush(wxColour(255, 255, 0)));  // 纯黄填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    // 绘制元件标识符
    wxFont font(wxFontInfo(6).Family(wxFONTFAMILY_SWISS).Bold());
    gc->SetFont(font, wxColour("#333333"));
    wxString text = self->identifier;
    double width, height, descent, externalLeading;
    gc->GetTextExtent(text, &width, &height, &descent, &externalLeading);
    double x = (m_bound.width - width) / 2.0;
    double y = (m_bound.height - height) / 2.0;

    gc->DrawText(text, x, y);

    // 绘制引脚标识符
    for (auto p : GetInputPins()) {
        gc->GetTextExtent(p.GetIdentifier(), &width, &height, &descent, &externalLeading);
        gc->DrawText(p.GetIdentifier(), p.pos.x- 5-width, p.pos.y);
    }

    for (auto p : GetOutputPins()) {
        gc->DrawText(p.GetIdentifier(), p.pos.x+5, p.pos.y);
    }

    for (auto p : GetInOutputPins()) {
        gc->DrawText(p.GetIdentifier(), p.pos.x, p.pos.y+5);
    }

    // -------------------------- 4. 恢复原始上下文变换 --------------------------
    gc->SetTransform(origMatrix); // 这里不用 *，直接传对象
}


/*
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
}*/

void TopModuleBox::Draw(wxGraphicsContext* gc) const
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
    for (const auto& pin : m_inPins) {
        gc->SetPen(wxPen(wxColour(0, 0, 255), 1.0)); // 蓝色边框
        gc->SetBrush(wxBrush(wxColour(0, 0, 255)));  // 蓝色填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    // 绘制输出引脚（红色圆点）
    for (const auto& pin : m_outPins) {
        gc->SetPen(wxPen(wxColour(255, 0, 0), 1.0)); // 红色边框
        gc->SetBrush(wxBrush(wxColour(255, 0, 0)));  // 红色填充
        gc->DrawEllipse(pin.pos.x - 3, pin.pos.y - 3, 6, 6); // 半径为3的圆点
    }

    // 绘制元件标识符
    wxFont font(wxFontInfo(6).Family(wxFONTFAMILY_SWISS).Bold());
    gc->SetFont(font, wxColour("#333333"));
    wxString text = self->identifier;
    double width, height, descent, externalLeading;
    gc->GetTextExtent(text, &width, &height, &descent, &externalLeading);

    gc->DrawText(text, m_bound.width/2 - width/2, -height);

    // 绘制引脚标识符
    for (auto p : GetInputPins()) {
        gc->GetTextExtent(p.GetIdentifier(), &width, &height, &descent, &externalLeading);
        gc->DrawText(p.GetIdentifier(), p.pos.x - 5 - width, p.pos.y);
    }

    for (auto p : GetOutputPins()) {
        gc->DrawText(p.GetIdentifier(), p.pos.x + 5, p.pos.y);
    }


    // -------------------------- 4. 恢复原始上下文变换 --------------------------
    gc->SetTransform(origMatrix); // 这里不用 *，直接传对象
}

// 改进后的构造函数
Pin::Pin(Point p, bool input) : pos(p), isInput(input), self(nullptr), top_self(nullptr) {
}

Pin::Pin(Point p, bool input, Port* s)
    : pos(p), isInput(input), self(s), top_self(nullptr) // 全部使用初始化列表
{
    if (s) this->identifier = (s->identifier);
    else this->identifier = "none";
}

Pin::Pin(Point p, bool input, SignalNode* s)
    : pos(p), isInput(input), self(nullptr), top_self(s) // 全部使用初始化列表
{
    if (s) this->identifier = (s->identifier);
    else this->identifier = "none";
}

void Pin::SetSelf(Port* s) {
    if (!s) return;
    self = s;
    identifier = s->identifier;
}

void Pin::SetSelf(SignalNode* s) {
    if (!s) return;
    top_self = s;
    identifier = s->identifier;
}

wxString Pin::GetIdentifier() { return identifier; };


Point Snap(const Point& pos) { return Point((pos.x + 20 / 2) / 20 * 20, (pos.y + 20 / 2) / 20 * 20); };
TopModuleBox::TopModuleBox(wxPoint start, wxPoint end, TopNode* self) :
    m_pos(start), self(self)
{
    int width = end.x - start.x;
    int height = end.y - start.y;

    // 1. 绘制矩形边框 (相对坐标)
    m_shapes.push_back(Line(Point(0, 0), Point(0, height)));
    m_shapes.push_back(Line(Point(0, 0), Point(width, 0)));
    m_shapes.push_back(Line(Point(width, 0), Point(width, height)));
    m_shapes.push_back(Line(Point(0, height), Point(width, height)));

    // 2. 添加左侧输入引脚 (均分算法)
    if (self->GetInPorts().size() > 0) {
        // 如果只有一个引脚，放中间；多个引脚则平分 height
        float segment = static_cast<float>(height) / (self->GetInPorts().size() + 1);
        for (int i = 0; i < self->GetInPorts().size(); ++i) {
            // 第 i 个引脚的位置在第 i+1 个等分点上
            int py = static_cast<int>((i + 1) * segment);
            m_inPins.push_back(Pin(Snap(Point(0, py)), true, self->GetInPorts()[i]));
        }
    }

    // 3. 添加右侧输出引脚 (均分算法)
    if (self->GetOutPorts().size() > 0) {
        float segment = static_cast<float>(height) / (self->GetOutPorts().size() + 1);
        for (int j = 0; j < self->GetOutPorts().size(); ++j) {
            int py = static_cast<int>((j + 1) * segment);
            m_outPins.push_back(Pin(Snap(Point(width, py)), false, self->GetOutPorts()[j]));
        }
    }

    m_bound = wxRect(0, 0, width, height); // 注意：m_bound 建议也用相对坐标，或根据 SetPos 统一
    SetPos(start);
}

wxString TopModuleBox::GetIdentifier() { return self->identifier; };

void TopModuleBox::SetEnd(wxPoint end) {
    int width = end.x - m_pos.x;
    int height = end.y - m_pos.y;

    // 1. 绘制矩形边框 (相对坐标)
    m_shapes.push_back(Line(Point(0, 0), Point(0, height)));
    m_shapes.push_back(Line(Point(0, 0), Point(width, 0)));
    m_shapes.push_back(Line(Point(width, 0), Point(width, height)));
    m_shapes.push_back(Line(Point(0, height), Point(width, height)));

    // 2. 添加左侧输入引脚 (均分算法)
    if (self->GetInPorts().size() > 0) {
        // 如果只有一个引脚，放中间；多个引脚则平分 height
        float segment = static_cast<float>(height) / (self->GetInPorts().size() + 1);
        for (int i = 0; i < self->GetInPorts().size(); ++i) {
            // 第 i 个引脚的位置在第 i+1 个等分点上
            int py = static_cast<int>((i + 1) * segment);
            m_inPins.push_back(Pin(Snap(Point(0, py)), true, self->GetInPorts()[i]));
        }
    }

    // 3. 添加右侧输出引脚 (均分算法)
    if (self->GetOutPorts().size() > 0) {
        float segment = static_cast<float>(height) / (self->GetOutPorts().size() + 1);
        for (int j = 0; j < self->GetOutPorts().size(); ++j) {
            int py = static_cast<int>((j + 1) * segment);
            m_outPins.push_back(Pin(Snap(Point(width, py)), false, self->GetOutPorts()[j]));
        }
    }

    m_bound = wxRect(0, 0, width, height); // 注意：m_bound 建议也用相对坐标，或根据 SetPos 统一
}
