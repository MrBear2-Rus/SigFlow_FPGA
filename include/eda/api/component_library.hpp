#pragma once

#include <string>
#include <vector>

namespace eda {

struct LibraryPoint {
    int x = 0;
    int y = 0;
};

// 元件模板的图形（wx-free 泛化表示；kind 对应 canvas_elements.json 的 shape type）。
struct LibraryShape {
    std::string kind;   // line / poly / circle / text / path / ArcShape / BezierShape / CubicBezierShape
    std::string color;
    std::vector<LibraryPoint> points;  // line: start,end；poly: 多点；bezier: 控制点
    int radius = 0;
    double startAngle = 0.0;
    double endAngle = 0.0;
    bool fill = false;
    std::string fillColor;
    std::string text;
    int fontSize = 10;
    std::string path;
    int strokeWidth = 1;
};

struct ComponentTemplate {
    std::string type;
    int width = 0;
    int height = 0;
    std::vector<LibraryPoint> inputPins;
    std::vector<LibraryPoint> outputPins;
    std::vector<LibraryShape> shapes;
};

// P2-5：元件库抽象（活路径 canvas_elements.json）。
class IComponentLibrary {
public:
    virtual ~IComponentLibrary() = default;
    virtual const std::vector<ComponentTemplate>& Components() const = 0;
    virtual const ComponentTemplate* Find(const std::string& type) const = 0;
};

} // namespace eda
