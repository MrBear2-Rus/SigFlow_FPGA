#pragma once

#include "DebugProtocol.h"
#include "TransportMetrics.h"

#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// 用户主动链路自测的结果状态。自测只做只读握手，不调用 RESET、CONFIG、ARM
// 或 READ，也不负责隐式打开/关闭传输；调用方负责设备控制权和生命周期。
enum class TransportSelfTestStatus {
    Passed,
    UserInitiatedRequired,
    CaptureActive,
    TransportClosed,
    UnsupportedProtocol,
    Failed,
};

const char* ToString(TransportSelfTestStatus status);

struct TransportSelfTestOptions {
    bool userInitiated = false;
    bool captureActive = false;
    std::string protocol = "full";
    int timeoutMs = 500;
    int attempts = 2;
};

struct TransportSelfTestResult {
    TransportSelfTestStatus status = TransportSelfTestStatus::Failed;
    bool passed = false;
    std::string backend;
    std::string protocol;
    std::string operation;
    std::string error;
    std::vector<std::string> diagnostics;
    ProtocolStats protocolStats;
    TransportStats transportStats;
};

class TransportSelfTestService {
public:
    // transport 必须已经打开。返回 false 表示自测被拒绝或握手失败；result
    // 始终包含可供 UI/日志展示的结构化状态和分层统计。
    static bool Run(ITransport& transport, const TransportSelfTestOptions& options,
                    TransportSelfTestResult& result);
};

} // namespace debug
} // namespace sigflow
