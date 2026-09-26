#include "GatewayDto.h"

#include <algorithm>

namespace eda {
namespace agent {
namespace {

const std::vector<CapabilityMap> kEducationCapabilities = {
    {"eda.sim.build", "sim.build"},
    {"eda.sim.run", "sim.run"},
    {"eda.synth", "synth"},
};

// 对教育 Agent 明确禁用（保留手动功能）的能力，reason 可读。
struct DisabledCapability {
    const char* id;
    const char* reason;
};

const DisabledCapability kDisabled[] = {
    {"eda.pnr", "place and route is not available to the educational agent"},
    {"eda.pack", "packing is not available to the educational agent"},
    {"eda.flash", "programming hardware is not available to the educational agent"},
    {"eda.debug", "hardware debug is not available to the educational agent"},
};

bool PluginProvides(const ReadyPlugin& plugin, const std::string& capability) {
    return std::find(plugin.capabilities.begin(), plugin.capabilities.end(), capability) !=
           plugin.capabilities.end();
}

} // namespace

const std::vector<CapabilityMap>& EducationCapabilities() { return kEducationCapabilities; }

std::vector<CapabilityStatus> ResolveEducationCapabilities(const std::vector<ReadyPlugin>& ready) {
    std::vector<CapabilityStatus> result;
    for (const auto& mapping : kEducationCapabilities) {
        CapabilityStatus status;
        status.id = mapping.capability;
        status.disabled = false;
        const ReadyPlugin* found = nullptr;
        for (const auto& plugin : ready) {
            if (PluginProvides(plugin, mapping.jobType) ||
                PluginProvides(plugin, mapping.capability)) {
                found = &plugin;
                break;
            }
        }
        if (found != nullptr) {
            status.ready = true;
            status.pluginId = found->id;
            status.pluginVersion = found->version;
            status.inputSchema = Json::object();
        } else {
            status.ready = false;
            status.reason = "no ready plugin provides jobType '" + mapping.jobType + "'";
        }
        result.push_back(std::move(status));
    }
    for (const auto& disabled : kDisabled) {
        CapabilityStatus status;
        status.id = disabled.id;
        status.ready = false;
        status.disabled = true;
        status.reason = disabled.reason;
        result.push_back(std::move(status));
    }
    return result;
}

Json BuildHealthData(const std::string& instanceId, const std::string& protocolVersion,
                     const std::string& edition, const std::string& build) {
    return Json{{"instance_id", instanceId},
                {"protocol_version", protocolVersion},
                {"edition", edition},
                {"build", build.empty() ? Json(nullptr) : Json(build)}};
}

Json BuildCapabilitiesData(const std::string& instanceId, const std::string& protocolVersion,
                           const std::string& edition, const std::string& build,
                           const std::vector<CapabilityStatus>& capabilities) {
    Json list = Json::array();
    for (const auto& capability : capabilities) {
        list.push_back(Json{{"id", capability.id},
                            {"plugin_id", capability.pluginId.empty()
                                              ? Json(nullptr)
                                              : Json(capability.pluginId)},
                            {"plugin_version", capability.pluginVersion.empty()
                                                   ? Json(nullptr)
                                                   : Json(capability.pluginVersion)},
                            {"ready", capability.ready},
                            {"disabled", capability.disabled},
                            {"reason", capability.reason.empty() ? Json(nullptr)
                                                                 : Json(capability.reason)},
                            {"input_schema", capability.inputSchema}});
    }
    Json data = BuildHealthData(instanceId, protocolVersion, edition, build);
    data["capabilities"] = list;
    return data;
}

Json SuccessEnvelope(const std::string& requestId, const std::string& traceId, const Json& data) {
    return Json{{"schema_version", "edu.api.v1"},
                {"request_id", requestId},
                {"trace_id", traceId},
                {"data", data}};
}

Json FailureEnvelope(const std::string& requestId, const std::string& traceId,
                     const std::string& code, const std::string& message, bool retryable) {
    return Json{{"schema_version", "edu.api.v1"},
                {"request_id", requestId},
                {"trace_id", traceId},
                {"error",
                 Json{{"code", code}, {"message", message}, {"retryable", retryable},
                      {"details", nullptr}}}};
}

} // namespace agent
} // namespace eda
