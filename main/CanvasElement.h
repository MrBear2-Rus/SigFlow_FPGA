#pragma once

#include <wx/wx.h>
#include <wx/dcgraph.h>  
#include <wx/graphics.h>
#include <wx/tokenzr.h>
#include <vector>
#include <variant>
#include "Wire.h"

struct Port;
class SecondNode;
class TopNode;

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
    bool isInput;
    wxString identifier;

    // 改进后的构造函数
    Pin(Point p, bool input);
    Pin(Point p, bool input, Port* s);
    Pin(Point p, bool input, SignalNode* s);
    void SetSelf(Port* s);
    void SetSelf(SignalNode* s);
    bool isOfTop() { return top_self != nullptr; };
    Port* GetSelfPort() const { return self; };
    SignalNode* GetSelfSignal() const { return top_self; }
    wxString GetIdentifier();

private:
    Port* self;
    SignalNode* top_self;
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


class TopModuleBox {
public:
    wxString type;
    wxPoint m_pos;
    wxRect m_bound;
    std::vector<Shape> m_shapes;

    TopNode* self;

    std::vector<Pin> m_inPins;
    std::vector<Pin> m_outPins;

    TopModuleBox() = default;
    TopModuleBox(wxPoint start, wxPoint end, TopNode* self);

    wxString GetType() { return type; };
    wxString GetIdentifier() ;
    const wxPoint& GetPos() const { return m_pos; }
    void SetPos(const wxPoint& p) { m_pos = p; }
    const std::vector<Shape>& GetShapes() const { return m_shapes; }
    wxRect GetBounds() const;
    void UpdateShapes(wxRect b, std::vector<Shape> sps) { m_bound = b; m_shapes = sps; };
    void SetEnd(wxPoint end);

    void AddInputPin(const Point& p) { m_inPins.push_back(Pin(p, true)); }
    void AddOutputPin(const Point& p) { m_outPins.push_back(Pin(p, false)); }
    void AddInputPin(const Point& p, Port* port) { m_inPins.push_back(Pin(p, true, port)); }
    void AddOutputPin(const Point& p, Port* port) { m_outPins.push_back(Pin(p, false, port)); }
    const std::vector<Pin>& GetInputPins() const { return m_inPins; }
    const std::vector<Pin>& GetOutputPins() const { return m_outPins; }

    std::vector<wxPoint> CalculateBezier(const Point& p0, const Point& p1, const Point& p2, int segments = 16) const;
    std::vector<wxPoint> CalculateCubicBezier(const Point& p0, const Point& p1, const Point& p2, const Point& p3, int segments = 32) const;

    void Draw(wxGraphicsContext* gc) const;
};

class SecondElement
{
public:
    wxString type;
    wxPoint m_pos;
    wxRect m_bound;
    std::vector<Shape> m_shapes;

    SecondNode* self;

    std::vector<Pin> m_inPins;
    std::vector<Pin> m_outPins;
    std::vector<Pin> m_inoutPins;

    SecondElement() = default;
    SecondElement(SecondNode* sn, std::vector<SecondElement>& templates);
    void MakeBlackBox();

    void SetSecondNode(SecondNode* sn) { self = sn; };
    wxString GetType() { return type; };
    wxString GetIdentifier() const;
    const wxPoint& GetPos() const { return m_pos; }
    void SetPos(const wxPoint& p) { m_pos = p;}
    const std::vector<Shape>& GetShapes() const { return m_shapes; }
    wxRect GetBounds() const;
    void UpdateShapes(wxRect b, std::vector<Shape> sps) { m_bound = b; m_shapes = sps; };

    void AddInputPin(const Point& p) { m_inPins.push_back(Pin(p, true)); }
    void AddOutputPin(const Point& p) { m_outPins.push_back(Pin(p, false)); }
    void AddInputPin(const Point& p, Port* port) { m_inPins.push_back(Pin(p, true, port)); }
    void AddOutputPin(const Point& p, Port* port) { m_outPins.push_back(Pin(p, false, port)); }
    void AddInOutputPin(const Point& p, Port* port) { m_inoutPins.push_back(Pin(p, false, port)); }
    const std::vector<Pin>& GetInputPins() const { return m_inPins; }
    const std::vector<Pin>& GetOutputPins() const { return m_outPins; }
    const std::vector<Pin>& GetInOutputPins() const { return m_inoutPins; }

    std::vector<wxPoint> CalculateBezier(const Point& p0, const Point& p1, const Point& p2, int segments = 16) const;
    std::vector<wxPoint> CalculateCubicBezier(const Point& p0, const Point& p1, const Point& p2, const Point& p3, int segments = 32) const;
    
    void Draw(wxGraphicsContext* gc) const;

};



