#include "CanvasModel.h"
#include "platform/PlatformPaths.h"
#include "CanvasElement.h"
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <json/json.h>
#include "my_log.h"

std::vector<SecondElement> g_elements;

std::vector<SecondElement> LoadSecondElements(const wxString& jsonPath)
{
    const std::filesystem::path modelPath(sigflow::platform::Utf8Path(jsonPath));
    std::ifstream f(modelPath, std::ios::binary);
    if (!f.is_open()) { MyLog("LoadCanvas: file not found!\n"); return {}; }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, f, &root, &errs)) return {};

    std::vector<SecondElement> out;
    for (const auto& elem : root) {

        SecondElement ce;
        ce.type = wxString::FromUTF8(elem["type"].asString());
        ce.m_bound = wxRect(0, 0, elem["bounds"][0].asInt(), elem["bounds"][1].asInt());


        for (const auto& pin : elem["inputPins"])
            ce.AddInputPin({ pin["x"].asInt(), pin["y"].asInt() });
        for (const auto& pin : elem["outputPins"])
            ce.AddOutputPin({ pin["x"].asInt(), pin["y"].asInt() });

        std::vector<Shape> shapes;
        for (const auto& shape : elem["shapes"]) {
            // asString() 返回 std::string，直接构造 wxString 会按 locale 解码；
            // 颜色字符串必须先校验再使用，否则非法值会生成无效 wxColour，
            // 交给 pen/brush 会在 GTK 上产生告警甚至断言。
            wxString type = wxString::FromUTF8(shape["type"].asString());
            const wxString colorText = wxString::FromUTF8(shape["color"].asString());
            wxColour color;
            if (!color.Set(colorText)) {
                color = *wxBLACK;
            }

            if (type == "polygon") {
                std::vector<Point> pts;
                for (const auto& pt : shape["points"])
                    pts.push_back({ pt[0].asInt(), pt[1].asInt() });
                shapes.push_back(PolyShape{ pts, color });
            }
            else if (type == "line") {
                int startX = shape["start"]["x"].asInt();
                int startY = shape["start"]["y"].asInt();
                int endX = shape["end"]["x"].asInt();
                int endY = shape["end"]["y"].asInt();
                // �����־��ȷ��ˮƽֱ�ߣ�y������ͬ��������
                /*MyLog("Loaded Line: start(%d,%d) �� end(%d,%d) [type: %s]\n",
                    startX, startY, endX, endY, type.ToUTF8().data());*/
                shapes.push_back(Line{
                    {startX, startY},
                    {endX, endY},
                    color
                    });
            }
            else if (type == "circle") {

                bool fill = shape.get("fill", false).asBool();
                wxColour fillColor;
                const wxString fillText = wxString::FromUTF8(
                    shape.get("fillColor", "#808080").asString());
                if (fill) {
                    if (!fillColor.Set(fillText)) fillColor = *wxBLACK;
                } else {
                    fillColor = wxColour(0, 0, 0);
                }

                shapes.push_back(Circle{
                    {shape["x"].asInt(), shape["y"].asInt()},
                    shape["r"].asInt(),
                    color,
                    fill,
                    fillColor
                    });
            }
            else if (type == "text") {
                wxString text = wxString::FromUTF8(shape["text"].asString());
                //text.Replace("&", "&&");
                // 缺省或非法字号会让 wxFont 变成无效字体（不显示/触发断言），至少钳到 1。
                const int fontSize = std::max(1, shape["fontSize"].asInt());
                shapes.push_back(Text{
                    {shape["x"].asInt(), shape["y"].asInt()},
                    text,

                    fontSize,
                    color
                    });
            }
            else if (type == "path") {
                wxColour stroke;
                if (!stroke.Set(wxString::FromUTF8(shape["stroke"].asString()))) {
                    stroke = *wxBLACK;
                }
                shapes.push_back(Path{
                    shape["d"].asString(),
                    stroke,
                    shape["strokeWidth"].asInt(),
                    shape["fill"].asString() != "none"
                    });
            }
            else if (type == "ArcShape") {
                shapes.push_back(ArcShape{
                    {shape["center"]["x"].asInt(), shape["center"]["y"].asInt()},
                    shape["radius"].asInt(),
                    shape["startAngle"].asDouble(),
                    shape["endAngle"].asDouble(),
                    color
                    });
            }

            // 椭半圆的绘制
            else if (type == "BezierShape") {
                Point p0 = { shape["p0"]["x"].asInt(), shape["p0"]["y"].asInt() };
                Point p1 = { shape["p1"]["x"].asInt(), shape["p1"]["y"].asInt() };
                Point p2 = { shape["p2"]["x"].asInt(), shape["p2"]["y"].asInt() };

                shapes.push_back(BezierShape{
                    p0, p1, p2,
                    color
                    });
            }
            // 三次贝塞尔曲线（4个控制点）
            else if (type == "CubicBezierShape") {
                Point p0 = { shape["p0"]["x"].asInt(), shape["p0"]["y"].asInt() };
                Point p1 = { shape["p1"]["x"].asInt(), shape["p1"]["y"].asInt() };
                Point p2 = { shape["p2"]["x"].asInt(), shape["p2"]["y"].asInt() };
                Point p3 = { shape["p3"]["x"].asInt(), shape["p3"]["y"].asInt() };

                shapes.push_back(CubicBezierShape{
                    p0, p1, p2, p3,
                    color
                    });
            }

        }
        ce.UpdateShapes(ce.m_bound, shapes);
        out.push_back(ce);
    }
    return out;
}
