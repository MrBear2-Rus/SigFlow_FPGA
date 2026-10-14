#pragma once

#include "CaptureRunService.h"
#include "DebugContract.h"
#include "SerialPortEnumerator.h"
#include "TransportBackend.h"

#include <functional>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// TB-32 的预检阶段。阶段是稳定的机器字段，UI 可以按阶段分组而不依赖
// 检查文案；检查本身可以由宿主、构建系统或设备适配器注册。
enum class PreflightPhase {
    Static,
    Capability,
    Build,
    Device,
};

enum class PreflightState {
    Passed,
    Warning,
    Failed,
    Skipped,
    Cancelled,
};

const char* ToString(PreflightPhase phase);
const char* ToString(PreflightState state);

struct PreflightInput {
    const DebugContract* contract = nullptr;
    const TransportOpenOptions* transport = nullptr;
    bool offlineOnly = false;
    std::function<bool()> isCancelled;
    // 可注入的只读设备探针。生产环境使用平台枚举/FTDI presence probe，
    // 测试和上层设备适配器可提供确定性实现；预检不会打开端口或抢占租约。
    std::function<std::vector<SerialPortInfo>()> enumerateSerialPorts;
    std::function<bool()> isFtdiPresent;
};

struct PreflightCheckResult {
    std::string id;
    PreflightPhase phase = PreflightPhase::Static;
    PreflightState state = PreflightState::Failed;
    bool requiresDevice = false;
    bool offlineSafe = true;
    std::string summary;
    std::vector<DiagnosticIssue> issues;
};

using PreflightCheckCallback =
    std::function<PreflightCheckResult(const PreflightInput&)>;

struct PreflightCheckDescriptor {
    std::string id;
    PreflightPhase phase = PreflightPhase::Static;
    bool requiresDevice = false;
    bool offlineSafe = true;
    PreflightCheckCallback callback;
};

struct PreflightReport {
    bool ready = false;
    bool cancelled = false;
    std::vector<PreflightCheckResult> checks;
    std::vector<DiagnosticIssue> issues;
};

// 可扩展的检查注册表。注册顺序不会影响结果顺序：执行时按阶段和 id
// 排序，保证日志、UI 与 CI 产物可重复比较。
class PreflightRegistry {
public:
    bool Register(PreflightCheckDescriptor descriptor, std::string& error);
    bool Run(const PreflightInput& input, PreflightReport& report,
             std::string& error) const;

    const std::vector<PreflightCheckDescriptor>& Checks() const { return checks_; }

private:
    std::vector<PreflightCheckDescriptor> checks_;
};

// TraceBridge 首版内置的检查集合。设备检查只做枚举和 presence probe，
// 不打开端口；offlineOnly=true 时仍可在没有板卡的情况下完成静态/能力预检。
PreflightRegistry MakeTraceBridgePreflightRegistry();

} // namespace debug
} // namespace sigflow
