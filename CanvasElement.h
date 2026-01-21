#pragma once

#include <wx/wx.h>
#include <wx/dcgraph.h>  
#include <wx/graphics.h>
#include <wx/tokenzr.h>
#include <vector>
#include <variant>
#include "Wire.h"

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
    LogicSignal s;
    Pin(Point p = Point(), wxString n = "", bool input = true)
        : pos(p), name(n), isInput(input), connectionWireId(-1), s(LogicSignal::E), isLeft(0){
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

// 闂ㄥ昂瀵稿弬鏁扮粨鏋勪綋
struct GateSizeParams {
    int width;
    int height;
    int pinSpacing;
    
    GateSizeParams(int w = 80, int h = 80, int ps = 40) 
        : width(w), height(h), pinSpacing(ps) {}
};

// 閫氱敤闂ㄥ睘鎬х粨鏋勪綋
struct GateProperties {
    wxString facing = "East";
    int dataBits = 1;
    wxString gateSize = "Medium";
    int numberOfInputs = 2;
    wxString label = "";
    wxString labelFont = "SansSerif Plain 12";
    std::vector<bool> negateInputs;
    
    GateProperties() {
        negateInputs.resize(32, false);
    }
    
    GateSizeParams GetSizeParams() const {
        if (gateSize == "Narrow") {
            return GateSizeParams(60, 60, 30);
        } else if (gateSize == "Wide") {
            return GateSizeParams(100, 100, 50);
        } else {
            return GateSizeParams(80, 80, 40);
        }
    }
    
    // 闂ㄧ殑楂樺害鏍规嵁杈撳叆鏁伴噺鍔ㄦ€佽绠?
    int GetActualHeight() const {
        GateSizeParams params = GetSizeParams();
        int extraInputs = numberOfInputs - 2;
        if (extraInputs < 0) extraInputs = 0;
        return params.height + extraInputs * params.pinSpacing;
    }
    
    // 寮曡剼闂磋窛淇濇寔鍥哄畾
    int GetActualPinSpacing() const {
        GateSizeParams params = GetSizeParams();
        return params.pinSpacing;
    }
    
    void Validate() {
        if (numberOfInputs < 2) numberOfInputs = 2;
        if (numberOfInputs > 32) numberOfInputs = 32;
        
        if (dataBits < 1) dataBits = 1;
        if (dataBits > 32) dataBits = 32;
        
        if (facing != "East" && facing != "West" && 
            facing != "North" && facing != "South") {
            facing = "East";
        }
        
        if (gateSize != "Narrow" && gateSize != "Medium" && gateSize != "Wide") {
            gateSize = "Medium";
        }
        
        if ((int)negateInputs.size() < numberOfInputs) {
            negateInputs.resize(numberOfInputs, false);
        }
    }
    
    wxFont GetLabelFontAsWxFont() const {
        wxString fontStr = labelFont;
        wxString family = "SansSerif";
        wxString style = "Plain";
        int size = 12;
        
        wxStringTokenizer tokenizer(fontStr, " ");
        if (tokenizer.HasMoreTokens()) family = tokenizer.GetNextToken();
        if (tokenizer.HasMoreTokens()) style = tokenizer.GetNextToken();
        if (tokenizer.HasMoreTokens()) {
            long sz;
            if (tokenizer.GetNextToken().ToLong(&sz)) size = (int)sz;
        }
        
        wxFontFamily wxFamily = wxFONTFAMILY_SWISS;
        if (family == "Serif" || family == "Times") wxFamily = wxFONTFAMILY_ROMAN;
        else if (family == "Monospaced" || family == "Courier") wxFamily = wxFONTFAMILY_TELETYPE;
        
        wxFontStyle wxStyle = wxFONTSTYLE_NORMAL;
        wxFontWeight wxWeight = wxFONTWEIGHT_NORMAL;
        if (style == "Bold") wxWeight = wxFONTWEIGHT_BOLD;
        else if (style == "Italic") wxStyle = wxFONTSTYLE_ITALIC;
        else if (style == "BoldItalic") { wxWeight = wxFONTWEIGHT_BOLD; wxStyle = wxFONTSTYLE_ITALIC; }
        
        return wxFont(size, wxFamily, wxStyle, wxWeight);
    }
    
    void SetLabelFontFromWxFont(const wxFont& font) {
        wxString family;
        switch (font.GetFamily()) {
            case wxFONTFAMILY_ROMAN: family = "Serif"; break;
            case wxFONTFAMILY_TELETYPE: family = "Monospaced"; break;
            default: family = "SansSerif"; break;
        }
        
        wxString style = "Plain";
        bool isBold = (font.GetWeight() == wxFONTWEIGHT_BOLD);
        bool isItalic = (font.GetStyle() == wxFONTSTYLE_ITALIC);
        if (isBold && isItalic) style = "BoldItalic";
        else if (isBold) style = "Bold";
        else if (isItalic) style = "Italic";
        
        labelFont = wxString::Format("%s %s %d", family, style, font.GetPointSize());
    }
};

using AndGateProperties = GateProperties;


class CanvasElement
{
private:
    wxString m_id;
    wxString m_name;
    wxPoint m_pos;
    wxPoint m_anchorPoint;
    int m_rotation = 0;
    std::vector<Shape> m_shapes;

    std::vector<LogicSignal> TruthTable;

    
    AndGateProperties m_andGateProps;
    GateProperties m_gateProps;
    
    void DrawVector(wxGCDC& gcdc) const;
    std::vector<wxPoint> CalculateBezier(const Point& p0, const Point& p1, const Point& p2, int segments = 16) const;
    std::vector<wxPoint> CalculateCubicBezier(const Point& p0, const Point& p1, const Point& p2, const Point& p3, int segments = 32) const;
    void DrawFallback(wxDC& dc) const;
    void DrawPathFallback(wxGCDC& gcdc, const Path& arg, std::function<wxPoint(const Point&)> off) const;
    void DrawPathFallback(wxDC& dc, const Path& arg, std::function<wxPoint(const Point&)> off) const;

public:

    void AddShape(const Shape& shape) { m_shapes.push_back(shape); }
    const wxString& GetName() const { return m_name; }
    void SetPos(const wxPoint& p) { m_pos = p; }
    const wxPoint& GetPos() const { return m_pos; }
    void SetRotation(int rotation) { m_rotation = rotation % 360; }
    int GetRotation() const { return m_rotation; }
    void SetAnchorPoint(const wxPoint& anchor) { m_anchorPoint = anchor; }
    const std::vector<Shape>& GetShapes() const { return m_shapes; }
    void Draw(wxDC& dc) const;
    wxRect GetBounds() const;
    void SetId(const wxString& id) { m_id = id; }
    const wxString& GetId() const { return m_id; }
    void AddInputPin(const Point& p, const wxString& name) { m_inputPins.push_back(Pin(p, name, true)); }
    void AddOutputPin(const Point& p, const wxString& name) { m_outputPins.push_back(Pin(p, name, false)); }


    void ReSetPinStatus();


    // 元件的引脚与状态管理，仿真相关

public:
    void initTruthTable();
    CanvasElement() = default;
    CanvasElement(const wxString& name, const wxPoint& pos);

    const std::vector<Pin>& GetInputPins() const { return m_inputPins; }
    const std::vector<Pin>& GetOutputPins() const { return m_outputPins; }

    void ToggleState() { m_state = !m_state; }
    bool GetState() const { return m_state; }
    void SetState(bool state) { m_state = state; }


    // 设置元件ID用于识别Pin_Input

    

    // 状态控制方法
    void SetOutputState(LogicSignal state);

    int GetOutputState() const { return m_outputState; }

    // AND闂ㄥ睘鎬ц闂柟娉?- 鍙湁 AND_Gate 闇€瑕佸睘鎬х紪杈?
    bool IsAndGate() const { return m_id == "AND_Gate"; }
    AndGateProperties& GetAndGateProps() { return m_andGateProps; }
    const AndGateProperties& GetAndGateProps() const { return m_andGateProps; }
    void SetAndGateProps(const AndGateProperties& props) { m_andGateProps = props; }
    
    // 閫氱敤閫昏緫闂ㄥ垽鏂拰灞炴€ц闂柟娉?
    bool IsLogicGate() const { 
        return m_id == "AND_Gate" || m_id == "AND_Gate_Rect" ||
               m_id == "OR_Gate" || m_id == "OR_Gate_Rect" ||
               m_id == "NAND_Gate" || m_id == "NAND_Gate_Rect" ||
               m_id == "NOR_Gate" || m_id == "NOR_Gate_Rect" ||
               m_id == "XOR_Gate" || m_id == "XOR_Gate_Rect" ||
               m_id == "XNOR_Gate" || m_id == "XNOR_Gate_Rect";
    }
    bool IsOrGate() const { return m_id == "OR_Gate" || m_id == "OR_Gate_Rect"; }
    GateProperties& GetGateProps() { return m_gateProps; }
    const GateProperties& GetGateProps() const { return m_gateProps; }
    void SetGateProps(const GateProperties& props) { m_gateProps = props; }
    
    // 鏍规嵁灞炴€ч噸鏂扮敓鎴愬舰鐘?
    void RegenerateShapes();
    
    // 涓篈ND闂ㄥ簲鐢ㄦ柟鍚戝彉鎹?
    void ApplyFacingTransform(const wxString& oldFacing, const wxString& newFacing);
    
    // 娓呴櫎鐜版湁褰㈢姸
    void ClearShapes() { m_shapes.clear(); }
    
    // 娓呴櫎寮曡剼
    void ClearPins() { m_inputPins.clear(); m_outputPins.clear(); }
    
    // 搴忓垪鍖?鍙嶅簭鍒楀寲闂ㄥ睘鎬?
    wxString SerializeGatePropsToJson() const;
    void DeserializeGatePropsFromJson(const wxString& json);

    std::vector<Pin> m_inputPins;
    std::vector<Pin> m_outputPins;

    // 添加状态和ID成员
    bool m_state = false; // 默认状态为0/false

    LogicSignal m_outputState = LogicSignal::E; 

    LogicSignal express(int input);
};
