#pragma once

#include "ITransport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// TB-10 后端选择输入。协议层仍由 DebugAcquisition 决定；本结构只描述
// “从哪里拿到透明字节流”，避免 GUI 直接依赖 VID/PID 和回退顺序。
struct TransportOpenOptions {
    std::string serialPort;
    std::uint32_t baud = 921600;
    bool preferFtdi = true; // false = explicitly select serial only
    bool allowSerialFallback = true;
};

enum class TransportDiagnosticCode {
    InvalidConfiguration,
    BackendUnavailable,
    DeviceMissing,
    OpenFailed,
    NoBackend,
    Opened,
};

const char* ToString(TransportDiagnosticCode code);

struct TransportDiagnostic {
    TransportDiagnosticCode code = TransportDiagnosticCode::OpenFailed;
    std::string backend;
    std::string message;
    bool retryable = false;
};

struct TransportOpenResult {
    std::unique_ptr<ITransport> transport;
    std::string backendId;       // ftdi:0403:6010:chB / serial:<port>
    std::vector<std::string> attempted;
    std::vector<std::string> diagnostics;
    std::vector<TransportDiagnostic> structuredDiagnostics;
};

// Snapshot of compile-time transport support used by the UI and support
// matrix.  It deliberately reports capability, not device presence: opening
// a backend remains an explicit operation with its own diagnostics.
struct TransportBuildCapabilities {
    bool serial = true;
    bool ftdi = false;
    std::string platform;
    std::string ftdiStatus;
};

TransportBuildCapabilities GetTransportBuildCapabilities();

// 统一创建并打开调试字节后端。失败时保留每一条尝试和可行动的原因；
// 不会在用户明确禁止回退时偷偷连接另一种物理后端。
bool OpenDebugTransport(const TransportOpenOptions& options,
                        TransportOpenResult& result,
                        std::string& error);

} // namespace debug
} // namespace sigflow
