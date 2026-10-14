#pragma once

#include "MonitorService.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow::debug {

struct IncidentEvidenceRef {
    std::string kind;       // capture / event / config / diagnostics / build / other
    std::string id;
    std::string path;
};

struct MonitorIncidentRequest {
    std::string incidentId;
    std::string triggerType;
    std::string conditionKey;
    std::string message;
    std::string eventId;
    std::string captureId;
    std::string buildIdentity;
    std::uint64_t observedMonotonicMs = 0;
    std::vector<IncidentEvidenceRef> evidence;
};

struct MonitorIncidentPolicy {
    std::uint64_t cooldownMs = 0;
    std::uint64_t maxSnapshots = 0; // 0 = unlimited
};

struct MonitorIncidentResult {
    bool created = false;
    bool deduplicated = false;
    bool rateLimited = false;
    std::string snapshotId;
    std::string snapshotPath;
    std::string fingerprint;
    std::vector<std::string> diagnostics;
};

// 生成轻量 incident 元数据，不复制波形/原始采集文件。引用会记录存在性，
// 使现场浏览器可以区分“已归档证据”和“当时不可用的证据”。
class MonitorIncidentService {
public:
    static bool Create(const MonitorPlan& plan,
                       const MonitorIncidentRequest& request,
                       const MonitorIncidentPolicy& policy,
                       MonitorIncidentResult& result,
                       std::string& error);
};

} // namespace sigflow::debug
