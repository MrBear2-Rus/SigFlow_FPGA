#include "DebugAcquisition.h"

#include "DebugSession.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "json/json.h"

namespace sigflow {
namespace debug {

namespace {

// intentParams JSON 中提取周期数（cycles/count/n），缺省 8。
std::uint16_t IntentCycles(const std::string& intentParams)
{
    Json::Value root;
    Json::Reader reader;
    if (!intentParams.empty() && reader.parse(intentParams, root) && root.isObject()) {
        for (const char* key : { "cycles", "count", "n" }) {
            if (root.isMember(key) && root[key].isInt() && root[key].asInt() > 0) {
                return static_cast<std::uint16_t>(root[key].asInt());
            }
        }
    }
    return 8;
}

std::string FingerprintHex(std::uint64_t value)
{
    char buf[24] = {};
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(value));
    return buf;
}

} // namespace

DebugAcquisition::DebugAcquisition(ITransport& transport, const DebugContract& contract)
    : transport_(transport), contract_(contract)
{
}

bool DebugAcquisition::TriggerParamsFromContract(const DebugContract& contract,
                                                 std::uint32_t& mask,
                                                 std::uint32_t& value,
                                                 std::uint8_t& mode,
                                                 std::uint16_t& count,
                                                 std::string& error)
{
    mask = 0;
    value = 0;
    mode = 0;
    count = 1;

    const std::string kind = contract.trigger.kind;
    const std::string intent = contract.trigger.intentKind;
    if (!intent.empty()) {
        if (intent == "state_stall") {
            mode = 1;
            count = IntentCycles(contract.trigger.intentParams);
            // 默认比较整条采样总线；契约可提供 mask 限定状态位子集。
            mask = 0xFFFFFFFF;
            if (!contract.trigger.mask.empty() &&
                !ParseHexU32(contract.trigger.mask, mask)) {
                error = "state_stall 的 mask 不是合法十六进制";
                return false;
            }
            return true;
        }
        if (intent == "handshake_timeout") {
            if (contract.trigger.hsValidPath.empty() ||
                contract.trigger.hsReadyPath.empty()) {
                error = "handshake_timeout 需要 trigger.hs_valid_path / hs_ready_path";
                return false;
            }
            mode = 2;
            count = IntentCycles(contract.trigger.intentParams);
            mask = 0xFFFFFFFF;
            return true;
        }
        error = "不支持的 intent: " + intent;
        return false;
    }
    if (kind.empty() || kind == "none") {
        return true;  // mask=0 立即触发
    }
    if (kind == "mask_equal") {
        if (!ParseHexU32(contract.trigger.mask, mask) ||
            !ParseHexU32(contract.trigger.value, value)) {
            error = "mask_equal 需要十六进制 mask/value";
            return false;
        }
        return true;
    }
    error = "RTL 尚未实现的触发类型: " + kind;
    return false;
}

bool DebugAcquisition::Acquire(const std::string& outDir, std::string& error,
                               const DebugAcquisitionOptions& options)
{
    const bool minimal = contract_.transport.protocol == "minimal";
    DebugInfo info;
    if (minimal) {
        // 最小协议没有 GET_INFO/指纹读回；构建产物由会话契约唯一标识。
        info.depth = static_cast<std::uint16_t>(contract_.capture.depth);
        info.width = 32;
        info.fingerprint64 = contract_.fingerprints.fingerprint64;
    } else {
        DebugProtocol protocol(transport_);
        protocol.SetAutoReconnect(options.autoReconnect);
        if (options.syncCalibrate && !protocol.SyncCalibrate(error)) {
            error = "同步校准失败: " + error;
            return false;
        }
        if (!protocol.GetInfo(info, error, options.commandTimeoutMs,
                              options.commandAttempts)) {
            error = "GET_INFO 失败: " + error;
            return false;
        }
        if (options.checkFingerprint &&
            info.fingerprint64 != contract_.fingerprints.fingerprint64) {
            error = "指纹不一致：板载 0x" + FingerprintHex(info.fingerprint64) +
                    "，契约 0x" + FingerprintHex(contract_.fingerprints.fingerprint64);
            return false;
        }
    }
    result_.fingerprint64 = info.fingerprint64;
    result_.depth = info.depth;

    std::uint32_t mask = 0, value = 0;
    std::uint8_t mode = 0;
    std::uint16_t count = 1;
    if (!TriggerParamsFromContract(contract_, mask, value, mode, count, error)) {
        error = "触发参数映射失败: " + error;
        return false;
    }
    DebugStatusInfo status;
    if (minimal) {
        if (contract_.capture.decimation != 1 || mode != 0 || count != 1) {
            error = "minimal UART 调试仅支持单次 mask_equal/none 触发和 decimation=1；"
                    "请选择 full 协议以使用扩展触发或抽取";
            return false;
        }
        MinimalDebugProtocol protocol(transport_);
        protocol.SetAutoReconnect(options.autoReconnect);
        if (!protocol.Configure(mask, value, error, options.commandTimeoutMs,
                                options.commandAttempts)) {
            error = "CONFIG 失败: " + error;
            return false;
        }
        if (!protocol.Arm(error, options.commandTimeoutMs, options.commandAttempts)) {
            error = "ARM 失败: " + error;
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options.captureTimeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!protocol.Status(status, error, options.commandTimeoutMs,
                                 options.commandAttempts)) {
                error = "STATUS 失败: " + error;
                return false;
            }
            if (status.flags & 0x40) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(options.pollIntervalMs));
        }
        if (!(status.flags & 0x40)) {
            error = "采集超时（busy 未在 " +
                    std::to_string(options.captureTimeoutMs) + " ms 内完成）";
            return false;
        }
        if (!protocol.ReadCapture(0, info.depth, result_.samples, error,
                                  options.commandTimeoutMs, options.commandAttempts)) {
            error = "读取采集数据失败: " + error;
            return false;
        }
    } else {
        DebugProtocol protocol(transport_);
        protocol.SetAutoReconnect(options.autoReconnect);
        if (!protocol.Configure(mask, value, contract_.capture.decimation, mode, count,
                                error, options.commandTimeoutMs, options.commandAttempts)) {
            error = "CONFIG 失败: " + error;
            return false;
        }
        if (!protocol.Arm(error, options.commandTimeoutMs, options.commandAttempts)) {
            error = "ARM 失败: " + error;
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options.captureTimeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!protocol.Status(status, error, options.commandTimeoutMs,
                                 options.commandAttempts)) {
                error = "STATUS 失败: " + error;
                return false;
            }
            if (status.flags & 0x40) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(options.pollIntervalMs));
        }
        if (!(status.flags & 0x40)) {
            error = "采集超时（busy 未在 " +
                    std::to_string(options.captureTimeoutMs) + " ms 内完成）";
            return false;
        }
        if (!protocol.ReadCapture(0, info.depth, result_.samples, error,
                                  options.commandTimeoutMs, options.commandAttempts)) {
            error = "读取采集数据失败: " + error;
            return false;
        }
    }
    result_.triggerIndex = status.triggerIndex;

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        error = "创建输出目录失败: " + ec.message();
        return false;
    }
    result_.rawPath = outDir + "\\capture.raw";
    result_.vcdPath = outDir + "\\capture.vcd";
    if (!SaveCaptureRaw(result_.rawPath, info.depth, info.width, 0, result_.samples,
                        error)) {
        return false;
    }
    std::vector<CaptureProbe> probes;
    probes.reserve(contract_.probes.size());
    for (const DebugProbe& p : contract_.probes) {
        probes.push_back({ p.id, p.path, p.width, p.bitOffset });
    }
    if (!DecodeCaptureToVcd(result_.samples, info.depth, status.triggerIndex, probes,
                            result_.vcdPath, result_.vcdMeta, error)) {
        error = "VCD 解码失败: " + error;
        return false;
    }
    return true;
}

bool DebugAcquisition::AcquireWithSession(const std::string& projectPath,
                                          const std::string& sessionId,
                                          const std::string& outDir, std::string& error,
                                          const DebugAcquisitionOptions& options)
{
    const bool ok = Acquire(outDir, error, options);
    DebugSessionService service;
    const DebugSessionState target =
        ok ? DebugSessionState::Captured
           : (error.find("超时") != std::string::npos ? DebugSessionState::TimedOut
                                                     : DebugSessionState::Failed);
    std::string transitionError;
    if (!service.Transition(projectPath, sessionId, target, ok ? "acquired" : error,
                            ok ? 0 : 1, transitionError)) {
        error = "会话状态迁移失败: " + transitionError;
        return false;
    }
    return ok;
}

} // namespace debug
} // namespace sigflow
