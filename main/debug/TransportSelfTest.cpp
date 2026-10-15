#include "TransportSelfTest.h"

#include <algorithm>

namespace sigflow {
namespace debug {

namespace {

void AddDiagnostic(TransportSelfTestResult& result, const std::string& code,
                   const std::string& message)
{
    result.diagnostics.push_back(code + ": " + message);
}

void SnapshotTransportStats(ITransport& transport, TransportSelfTestResult& result)
{
    if (auto* measured = dynamic_cast<InstrumentedTransport*>(&transport)) {
        result.transportStats = measured->Stats();
    }
}

} // namespace

const char* ToString(TransportSelfTestStatus status)
{
    switch (status) {
        case TransportSelfTestStatus::Passed: return "PASSED";
        case TransportSelfTestStatus::UserInitiatedRequired: return "USER_INITIATED_REQUIRED";
        case TransportSelfTestStatus::CaptureActive: return "CAPTURE_ACTIVE";
        case TransportSelfTestStatus::TransportClosed: return "TRANSPORT_CLOSED";
        case TransportSelfTestStatus::UnsupportedProtocol: return "UNSUPPORTED_PROTOCOL";
        case TransportSelfTestStatus::Failed: return "FAILED";
    }
    return "FAILED";
}

bool TransportSelfTestService::Run(ITransport& transport,
                                   const TransportSelfTestOptions& options,
                                   TransportSelfTestResult& result)
{
    result = TransportSelfTestResult();
    result.protocol = options.protocol;
    result.operation = options.protocol == "minimal" ? "STATUS" : "PING";

    // These gates deliberately precede any I/O. A programmatic caller cannot
    // accidentally turn a background health check into a user-visible probe.
    if (!options.userInitiated) {
        result.status = TransportSelfTestStatus::UserInitiatedRequired;
        result.error = "transport self-test requires an explicit user action";
        AddDiagnostic(result, "USER_INITIATED_REQUIRED", result.error);
        return false;
    }
    if (options.captureActive) {
        result.status = TransportSelfTestStatus::CaptureActive;
        result.error = "transport self-test is blocked while capture is active";
        AddDiagnostic(result, "CAPTURE_ACTIVE", result.error);
        return false;
    }
    if (!transport.IsOpen()) {
        result.status = TransportSelfTestStatus::TransportClosed;
        result.error = "transport self-test requires an already-open transport";
        AddDiagnostic(result, "TRANSPORT_CLOSED", result.error);
        return false;
    }
    if (options.protocol != "full" && options.protocol != "minimal") {
        result.status = TransportSelfTestStatus::UnsupportedProtocol;
        result.error = "unsupported TraceBridge protocol: " + options.protocol;
        AddDiagnostic(result, "UNSUPPORTED_PROTOCOL", result.error);
        return false;
    }

    const int timeoutMs = std::max(1, options.timeoutMs);
    const int attempts = std::max(1, options.attempts);
    std::string error;
    bool passed = false;
    if (options.protocol == "full") {
        DebugProtocol protocol(transport);
        std::uint8_t version = 0;
        passed = protocol.Ping(version, error, timeoutMs, attempts);
        result.protocolStats = protocol.Stats();
        if (passed) {
            result.diagnostics.push_back("PING responded; no DUT reset or capture mutation issued");
        }
    } else {
        // minimal 协议没有 PING；STATUS 是只读命令，不会改变触发器、采样
        // 配置或采集存储，因此可作为同等安全的链路探针。
        MinimalDebugProtocol protocol(transport);
        DebugStatusInfo status;
        passed = protocol.Status(status, error, timeoutMs, attempts);
        result.protocolStats = protocol.Stats();
        if (passed) {
            result.diagnostics.push_back("STATUS responded; no DUT reset or capture mutation issued");
        }
    }

    SnapshotTransportStats(transport, result);
    if (passed) {
        result.status = TransportSelfTestStatus::Passed;
        result.passed = true;
        return true;
    }

    result.status = TransportSelfTestStatus::Failed;
    result.error = error.empty() ? "TraceBridge self-test handshake failed" : error;
    AddDiagnostic(result, "HANDSHAKE_FAILED", result.error);
    return false;
}

} // namespace debug
} // namespace sigflow
