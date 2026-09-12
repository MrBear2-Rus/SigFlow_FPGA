#include "CanvasModel.h"
#include "CanvasElement.h"
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <fstream>
#include <json/json.h>
#include "my_log.h"

std::vector<SecondElement> g_elements;

std::vector<SecondElement> LoadSecondElements(const wxString& jsonPath)
{
    std::ifstream f(jsonPath.ToStdString(), std::ios::binary);
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
            wxColour color(shape["color"].asString());
            wxString type = shape["type"].asString();

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
                wxColour fillColor = fill ? wxColour(shape.get("fillColor", "#808080").asString()) : wxColour(0, 0, 0);

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
                shapes.push_back(Text{
                    {shape["x"].asInt(), shape["y"].asInt()},
                    text,

                    shape["fontSize"].asInt(),
                    color
                    });
            }
            else if (type == "path") {
                shapes.push_back(Path{
                    shape["d"].asString(),
                    wxColour(shape["stroke"].asString()),
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
