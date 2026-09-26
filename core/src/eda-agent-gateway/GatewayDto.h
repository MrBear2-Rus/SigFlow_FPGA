#pragma once

#include <eda/api/Types.h>

#include <string>
#include <vector>

namespace eda {
namespace agent {

// 教育版 Agent 能力白名单（spec §5.2）：网关稳定名 → 插件实际 jobType。
struct CapabilityMap {
    std::string capability;  // eda.sim.build / eda.sim.run / eda.synth
    std::string jobType;     // sim.build / sim.run / synth
};

const std::vector<CapabilityMap>& EducationCapabilities();

// 单个能力的状态（供 GET /capabilities）。
struct CapabilityStatus {
    std::string id;
    std::string pluginId;
    std::string pluginVersion;
    bool ready = false;
    bool disabled = false;
    std::string reason;
    Json inputSchema = Json{};
};

// 能力解析：从一批"已就绪插件"（id/version/capabilities）推导教育能力可用性。
// 插件以 (id, version, capabilities) 描述，避免依赖 wx/Composer。
struct ReadyPlugin {
    std::string id;
    std::string version;
    std::vector<std::string> capabilities;
};

std::vector<CapabilityStatus> ResolveEducationCapabilities(const std::vector<ReadyPlugin>& ready);

// GET /health 的 data 载荷。
Json BuildHealthData(const std::string& instanceId, const std::string& protocolVersion,
                     const std::string& edition, const std::string& build);

// GET /capabilities 的 data 载荷（含 instance/protocol/edition/build + capabilities[]）。
Json BuildCapabilitiesData(const std::string& instanceId, const std::string& protocolVersion,
                           const std::string& edition, const std::string& build,
                           const std::vector<CapabilityStatus>& capabilities);

// 统一成功/失败信封（spec §5.1）。
Json SuccessEnvelope(const std::string& requestId, const std::string& traceId, const Json& data);
Json FailureEnvelope(const std::string& requestId, const std::string& traceId,
                     const std::string& code, const std::string& message, bool retryable);

} // namespace agent
} // namespace eda
