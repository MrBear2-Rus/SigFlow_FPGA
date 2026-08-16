#include "DebugNetlistValidator.h"

#include <json/json.h>

#include <map>
#include <memory>

namespace sigflow {
namespace debug {

namespace {

bool ParseJson(const std::string& content, Json::Value& root, std::string& error)
{
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string parseErrors;
    if (!reader->parse(content.data(), content.data() + content.size(), &root,
                       &parseErrors)) {
        error = "Yosys JSON parse error: " + parseErrors;
        return false;
    }
    if (!root.isObject() || !root["modules"].isObject()) {
        error = "Yosys JSON missing 'modules'";
        return false;
    }
    return true;
}

bool FindModule(const Json::Value& root, const std::string& topModule,
                const Json::Value*& module, std::string& error)
{
    const Json::Value& modules = root["modules"];
    if (!modules.isMember(topModule)) {
        error = "top module '" + topModule + "' not found in netlist";
        return false;
    }
    module = &modules[topModule];
    return true;
}

// 规范化网名：去掉前导反斜杠
std::string NormalizeNetName(const std::string& name)
{
    if (!name.empty() && name[0] == '\\') return name.substr(1);
    return name;
}

} // namespace

bool DebugNetlistValidator::ParseTopPorts(const std::string& jsonContent,
                                          const std::string& topModule,
                                          std::vector<DebugPortInfo>& ports,
                                          std::string& error)
{
    ports.clear();
    Json::Value root;
    if (!ParseJson(jsonContent, root, error)) return false;
    const Json::Value* module = nullptr;
    if (!FindModule(root, topModule, module, error)) return false;

    const Json::Value& portList = (*module)["ports"];
    if (!portList.isObject()) return true;
    for (auto it = portList.begin(); it != portList.end(); ++it) {
        DebugPortInfo info;
        info.name = it.name();
        const Json::Value& port = *it;
        info.direction = port.get("direction", "").asString();
        info.width = port["bits"].isArray()
                         ? static_cast<int>(port["bits"].size())
                         : 1;
        ports.push_back(std::move(info));
    }
    return true;
}

bool DebugNetlistValidator::ValidateProbes(const std::string& jsonContent,
                                           const std::string& topModule,
                                           const std::vector<DebugProbe>& probes,
                                           std::string& error)
{
    Json::Value root;
    if (!ParseJson(jsonContent, root, error)) return false;
    const Json::Value* module = nullptr;
    if (!FindModule(root, topModule, module, error)) return false;

    // 顶层端口名 → width
    std::map<std::string, int> portWidths;
    const Json::Value& portList = (*module)["ports"];
    if (portList.isObject()) {
        for (auto it = portList.begin(); it != portList.end(); ++it) {
            portWidths[it.name()] = it->get("bits", Json::Value(Json::arrayValue)).isArray()
                                        ? static_cast<int>((*it)["bits"].size())
                                        : 1;
        }
    }

    // 网名 → width（扫描所有模块：探针可能位于用户 DUT 子模块）
    // Yosys JSON 的网表键为 "netnames"。
    std::map<std::string, int> netWidths;
    const Json::Value& modules = root["modules"];
    if (modules.isObject()) {
        for (auto modIt = modules.begin(); modIt != modules.end(); ++modIt) {
            const Json::Value& nets = (*modIt)["netnames"];
            if (!nets.isObject()) continue;
            for (auto it = nets.begin(); it != nets.end(); ++it) {
                const std::string name = NormalizeNetName(it.name());
                netWidths[name] = it->get("bits", Json::Value(Json::arrayValue)).isArray()
                                      ? static_cast<int>((*it)["bits"].size())
                                      : 1;
            }
        }
    }

    std::string failures;
    for (const DebugProbe& probe : probes) {
        // 叶子名 = 路径最后一段
        std::string leaf = probe.path;
        const std::size_t dot = leaf.find_last_of('.');
        if (dot != std::string::npos) leaf = leaf.substr(dot + 1);

        int foundWidth = -1;
        auto portIt = portWidths.find(probe.path);
        if (portIt == portWidths.end()) portIt = portWidths.find(leaf);
        if (portIt != portWidths.end()) {
            foundWidth = portIt->second;
        } else {
            // 网表按叶子名或 ".leaf" 后缀匹配
            for (const auto& [name, width] : netWidths) {
                if (name == leaf || (name.size() > leaf.size() &&
                                     name.compare(name.size() - leaf.size() - 1,
                                                  std::string::npos, "." + leaf) == 0)) {
                    foundWidth = width;
                    break;
                }
            }
        }

        if (foundWidth < 0) {
            failures += "  probe '" + probe.id + "' (" + probe.path +
                        ") not found in netlist\n";
        } else if (foundWidth != static_cast<int>(probe.width)) {
            failures += "  probe '" + probe.id + "' width mismatch: expected " +
                        std::to_string(probe.width) + ", got " +
                        std::to_string(foundWidth) + "\n";
        }
    }

    if (!failures.empty()) {
        error = "probe validation failed:\n" + failures;
        return false;
    }
    return true;
}

} // namespace debug
} // namespace sigflow
