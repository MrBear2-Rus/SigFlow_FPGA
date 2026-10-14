#pragma once

#include "CaptureRunService.h"

#include <string>
#include <vector>

namespace sigflow::debug {

enum class RecoveryActionKind {
    SelectDevice,
    ReleaseControl,
    ReduceProbeCount,
    OpenTimingReport,
    RearmCapture,
};

const char* ToString(RecoveryActionKind kind);

struct TraceBridgeRecoveryContext {
    std::vector<DiagnosticIssue> issues;
    bool captureActive = false;
    bool deviceAvailable = false;
    bool ownsControl = false;
    bool hasProbePlan = false;
    bool hasTimingReport = false;
};

struct TraceBridgeRecoveryAction {
    std::string id;
    RecoveryActionKind kind = RecoveryActionKind::SelectDevice;
    bool available = false;
    bool requiresUserConfirmation = false;
    bool changesDut = false;
    std::string summary;
    std::vector<std::string> issueCodes;
};

struct TraceBridgeRecoveryPlan {
    std::vector<TraceBridgeRecoveryAction> actions;
};

// 将采集/预检诊断映射为稳定的宿主恢复动作。该服务只生成计划，不打开设备、
// 不释放租约、不修改契约，也不向 DUT 发送 RESET/ARM 等命令。
class TraceBridgeRecoveryService {
public:
    static bool Build(const TraceBridgeRecoveryContext& context,
                      TraceBridgeRecoveryPlan& plan,
                      std::string& error);
};

} // namespace sigflow::debug
