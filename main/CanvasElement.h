#pragma once

#include <wx/wx.h>
#include <wx/dcgraph.h>  
#include <wx/graphics.h>
#include <wx/tokenzr.h>
#include <vector>
#include <variant>
#include "Wire.h"

class SecondNode;

struct Point {
    int x, y;
    Point(int x = 0, int y = 0) : x(x), y(y) {}
};

struct Line {
    Point start, end;
    wxColour color;
    Line(Point s = Point(), Point e = Point(), wxColour c = wxColour(0, 0, 0))
        : start(s), end(e), color(c) {
    }
};

struct PolyShape {
    std::vector<Point> pts;
    wxColour color;
    PolyShape(std::vector<Point> p = {}, wxColour c = wxColour(0, 0, 0))
        : pts(p), color(c) {
    }
};

struct Circle {
    Point center;
    int radius;
    wxColour color;

    bool fill;
    wxColour fillColor;
    Circle(Point c = Point(), int r = 0, wxColour col = wxColour(0, 0, 0),
        bool f = false, wxColour fc = wxColour(128, 128, 128))
        : center(c), radius(r), color(col), fill(f), fillColor(fc) {

    }
};

struct Text {
    Point pos;
    wxString text;
    int fontSize;
    wxColour color;
    Text(Point p = Point(), wxString t = "", int fs = 10, wxColour c = wxColour(0, 0, 0))
        : pos(p), text(t), fontSize(fs), color(c) {
    }
};

struct Pin {
    Point pos;
    wxString name;
    bool isInput;
    int connectionWireId;
    bool isLeft;
    Pin(Point p = Point(), wxString n = "", bool input = true)
        : pos(p), name(n), isInput(input), connectionWireId(-1), isLeft(0){
    }
};

struct ArcShape {
    Point center;
    int radius;
    double startAngle;
    double endAngle;
    wxColour color;
    ArcShape(Point c = Point(), int r = 0, double sa = 0, double ea = 0, wxColour col = wxColour(0, 0, 0))
        : center(c), radius(r), startAngle(sa), endAngle(ea), color(col) {
    }
};

struct BezierShape {
    Point p0, p1, p2;
    wxColour color;
    BezierShape(Point p0 = Point(), Point p1 = Point(), Point p2 = Point(), wxColour c = wxColour(0, 0, 0))
        : p0(p0), p1(p1), p2(p2), color(c) {
    }
};

// 三次贝塞尔曲线（4个控制点）
struct CubicBezierShape {
    Point p0, p1, p2, p3;
    wxColour color;
    CubicBezierShape(Point p0 = Point(), Point p1 = Point(), Point p2 = Point(), Point p3 = Point(), wxColour c = wxColour(0, 0, 0))
        : p0(p0), p1(p1), p2(p2), p3(p3), color(c) {
    }
};

struct Path {
    std::string d;
    wxColour stroke;
    int strokeWidth;
    bool fill;
    Path(std::string d = "", wxColour s = wxColour(0, 0, 0), int sw = 1, bool f = false)
        : d(d), stroke(s), strokeWidth(sw), fill(f) {
    }
};

using Shape = std::variant<Line, PolyShape, Circle, Text, Path, ArcShape, BezierShape, CubicBezierShape>;


class CanvasElement
{
public:
    wxString m_id;
    wxPoint m_pos;
    std::vector<Shape> m_shapes;

    SecondNode* self;

    wxString GetName() { return m_id; };
    void SetId(wxString id) { m_id = id; };
    std::vector<wxPoint> CalculateBezier(const Point& p0, const Point& p1, const Point& p2, int segments = 16) const;
    std::vector<wxPoint> CalculateCubicBezier(const Point& p0, const Point& p1, const Point& p2, const Point& p3, int segments = 32) const;

public:

    void AddShape(const Shape& shape) { m_shapes.push_back(shape); }
    void SetPos(const wxPoint& p) { m_pos = p; }
    const wxPoint& GetPos() const { return m_pos; }
    const std::vector<Shape>& GetShapes() const { return m_shapes; }
    virtual void Draw(wxGraphicsContext* gc) const;
    wxRect GetBounds() const;
    void AddInputPin(const Point& p, const wxString& name) { m_inputPins.push_back(Pin(p, name, true)); }
    void AddOutputPin(const Point& p, const wxString& name) { m_outputPins.push_back(Pin(p, name, false)); }


    CanvasElement() = default;
    CanvasElement(const wxPoint& pos);
    const std::vector<Pin>& GetInputPins() const { return m_inputPins; }
    const std::vector<Pin>& GetOutputPins() const { return m_outputPins; }
    

    std::vector<Pin> m_inputPins;
    std::vector<Pin> m_outputPins;

};


class InputElement : public CanvasElement {
    wxString m_id;
    bool m_state;
    void Draw(wxGraphicsContext* gc) const override;
};

class TopModuleBox : public CanvasElement {
};
