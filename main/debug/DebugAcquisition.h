#pragma once

#include "CaptureDecoder.h"
#include "DebugContract.h"
#include "DebugProtocol.h"
#include "ITransport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// 端到端采集流程（T-P3-04 核心，非 GUI）：
// 同步校准 → GET_INFO 指纹校验 → CONFIG → ARM → 轮询 STATUS → 分块读取
// → 解码 VCD + 保存 capture.raw。
struct DebugAcquisitionOptions {
    bool syncCalibrate = true;
    bool checkFingerprint = true;
    bool autoReconnect = true;
    int pollIntervalMs = 20;
    int captureTimeoutMs = 5000;
    int commandTimeoutMs = 1000;
    int commandAttempts = 3;
};

struct DebugAcquisitionResult {
    std::vector<std::uint32_t> samples;  // 环形顺序（地址序 = 相对触发时间序）
    std::uint16_t triggerIndex = 0;
    std::uint64_t fingerprint64 = 0;
    std::uint16_t depth = 0;
    CaptureVcdMeta vcdMeta;
    std::string rawPath;
    std::string vcdPath;
};

class DebugAcquisition {
public:
    DebugAcquisition(ITransport& transport, const DebugContract& contract);

    // 采集到 outDir（capture.raw / capture.vcd）。
    bool Acquire(const std::string& outDir, std::string& error,
                 const DebugAcquisitionOptions& options = {});

    // 采集并驱动会话状态（Armed → Captured / TimedOut / Failed）。
    bool AcquireWithSession(const std::string& projectPath, const std::string& sessionId,
                            const std::string& outDir, std::string& error,
                            const DebugAcquisitionOptions& options = {});

    const DebugAcquisitionResult& Result() const { return result_; }

    // 契约触发 → RTL trigger_mode/mask/value/count 映射。
    static bool TriggerParamsFromContract(const DebugContract& contract,
                                          std::uint32_t& mask, std::uint32_t& value,
                                          std::uint8_t& mode, std::uint16_t& count,
                                          std::string& error);

private:
    ITransport& transport_;
    const DebugContract& contract_;
    DebugAcquisitionResult result_;
};

} // namespace debug
} // namespace sigflow
