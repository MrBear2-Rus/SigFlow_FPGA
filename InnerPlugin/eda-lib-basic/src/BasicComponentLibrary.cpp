#include "BasicComponentLibrary.h"

#include <eda/api/Types.h>

#include <fstream>
#include <iterator>

namespace eda {
namespace lib {
namespace {

int IntOr(const Json& object, const char* key, int fallback) {
    if (object.is_object() && object.contains(key) && object[key].is_number()) {
        return object[key].get<int>();
    }
    return fallback;
}

std::string StringOr(const Json& object, const char* key, const std::string& fallback = {}) {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return fallback;
}

double DoubleOr(const Json& object, const char* key, double fallback) {
    if (object.is_object() && object.contains(key) && object[key].is_number()) {
        return object[key].get<double>();
    }
    return fallback;
}

LibraryPoint ReadPoint(const Json& object) {
    LibraryPoint point;
    point.x = IntOr(object, "x", 0);
    point.y = IntOr(object, "y", 0);
    return point;
}

LibraryShape ReadShape(const Json& shape) {
    LibraryShape result;
    result.kind = StringOr(shape, "type");
    result.color = StringOr(shape, "color", StringOr(shape, "stroke", "#000000"));
    result.fillColor = StringOr(shape, "fillColor");
    result.fill = shape.is_object() && shape.contains("fill") && shape["fill"].is_boolean()
                      ? shape["fill"].get<bool>()
                      : false;
    result.radius = IntOr(shape, "r", IntOr(shape, "radius", 0));
    result.startAngle = DoubleOr(shape, "startAngle", 0.0);
    result.endAngle = DoubleOr(shape, "endAngle", 0.0);
    result.text = StringOr(shape, "text");
    result.fontSize = IntOr(shape, "fontSize", 10);
    result.path = StringOr(shape, "d");
    result.strokeWidth = IntOr(shape, "strokeWidth", 1);

    if (shape.is_object()) {
        if (shape.contains("start")) result.points.push_back(ReadPoint(shape["start"]));
        if (shape.contains("end")) result.points.push_back(ReadPoint(shape["end"]));
        if (shape.contains("center")) result.points.push_back(ReadPoint(shape["center"]));
        if (shape.contains("pts") && shape["pts"].is_array()) {
            for (const auto& pt : shape["pts"]) result.points.push_back(ReadPoint(pt));
        }
        for (const char* key : {"p0", "p1", "p2", "p3"}) {
            if (shape.contains(key)) result.points.push_back(ReadPoint(shape[key]));
        }
    }
    return result;
}

ComponentTemplate ReadComponent(const Json& component) {
    ComponentTemplate result;
    result.type = StringOr(component, "type");
    if (component.contains("bounds") && component["bounds"].is_array() &&
        component["bounds"].size() >= 2) {
        result.width = component["bounds"][0].get<int>();
        result.height = component["bounds"][1].get<int>();
    }
    if (component.contains("inputPins") && component["inputPins"].is_array()) {
        for (const auto& pin : component["inputPins"]) result.inputPins.push_back(ReadPoint(pin));
    }
    if (component.contains("outputPins") && component["outputPins"].is_array()) {
        for (const auto& pin : component["outputPins"]) result.outputPins.push_back(ReadPoint(pin));
    }
    if (component.contains("shapes") && component["shapes"].is_array()) {
        for (const auto& shape : component["shapes"]) result.shapes.push_back(ReadShape(shape));
    }
    return result;
}

} // namespace

bool BasicComponentLibrary::Parse(const std::string& json, BasicComponentLibrary& library,
                                  std::string& error) {
    Json root;
    try {
        root = Json::parse(json);
    } catch (const std::exception& parseError) {
        error = std::string("invalid component library JSON: ") + parseError.what();
        return false;
    }
    if (!root.is_array()) {
        error = "component library must be a JSON array";
        return false;
    }
    std::vector<ComponentTemplate> components;
    for (const auto& entry : root) {
        if (!entry.is_object()) continue;
        ComponentTemplate component = ReadComponent(entry);
        if (!component.type.empty()) components.push_back(std::move(component));
    }
    if (components.empty()) {
        error = "component library has no components";
        return false;
    }
    library.components_ = std::move(components);
    return true;
}

bool BasicComponentLibrary::LoadFile(const std::filesystem::path& file,
                                     BasicComponentLibrary& library, std::string& error) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "component library not found: " + file.string();
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    return Parse(content, library, error);
}

const ComponentTemplate* BasicComponentLibrary::Find(const std::string& type) const {
    for (const auto& component : components_) {
        if (component.type == type) return &component;
    }
    return nullptr;
}

} // namespace lib
} // namespace eda
