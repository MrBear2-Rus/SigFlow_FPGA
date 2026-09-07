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

bool DebugAcquisition::IsAborted(const DebugAcquisitionOptions& options)
{
    return options.aborted != nullptr && *options.aborted;
}

void DebugAcquisition::FireProgress(const DebugAcquisitionOptions& options,
                                    DebugAcqStage stage, int percentInStage,
                                    const std::string& message) const
{
    if (options.onProgress) {
        try {
            options.onProgress(stage, percentInStage, message);
        } catch (...) {
            // 回调抛异常不影响采集流程
        }
    }
}

const char* DebugAcquisition::StageName(DebugAcqStage stage)
{
    switch (stage) {
        case DebugAcqStage::Idle:         return "Idle";
        case DebugAcqStage::SyncCalibrate:return "Sync";
        case DebugAcqStage::GetInfo:      return "GetInfo";
        case DebugAcqStage::MapTrigger:   return "MapTrigger";
        case DebugAcqStage::Configure:    return "Config";
        case DebugAcqStage::Arm:          return "Arm";
        case DebugAcqStage::PollStatus:   return "PollStatus";
        case DebugAcqStage::ReadSamples:  return "ReadSamples";
        case DebugAcqStage::SaveRaw:      return "SaveRaw";
        case DebugAcqStage::DecodeVcd:    return "DecodeVCD";
        case DebugAcqStage::Done:         return "Done";
    }
    return "?";
}

int DebugAcquisition::StageOrder(DebugAcqStage stage)
{
    return static_cast<int>(stage);
}

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
        return true;
    }
    if (kind == "mask_equal") {
        if (!ParseHexU32(contract.trigger.mask, mask) ||
            !ParseHexU32(contract.trigger.value, value)) {
            error = "mask_equal 需要十六进制 mask/value";
            return false;
        }
        return true;
    }
    if (kind == "edge_rising") {
        // 边沿上升：mask 指定监听位，value 设为掩码值（0→1 检测）
        mask = 0xFFFFFFFF;
        if (!contract.trigger.mask.empty() &&
            !ParseHexU32(contract.trigger.mask, mask)) {
            error = "edge_rising 的 mask 不是合法十六进制";
            return false;
        }
        mode = 3;  // RTL trigger_mode 3 = edge_rising
        value = mask; // 期望值 = mask 本身（0→1 边沿）
        return true;
    }
    if (kind == "edge_falling") {
        mask = 0xFFFFFFFF;
        if (!contract.trigger.mask.empty() &&
            !ParseHexU32(contract.trigger.mask, mask)) {
            error = "edge_falling 的 mask 不是合法十六进制";
            return false;
        }
        mode = 4;  // RTL trigger_mode 4 = edge_falling
        value = 0;  // 期望值 = 0（1→0 边沿）
        return true;
    }
    error = "RTL 尚未实现的触发类型: " + kind;
    return false;
}

bool DebugAcquisition::Acquire(const std::string& outDir, std::string& error,
                               const DebugAcquisitionOptions& options)
{
    if (IsAborted(options)) { error = "Aborted"; return false; }

    const bool minimal = contract_.transport.protocol == "minimal";
    DebugInfo info;
    if (minimal) {
        info.depth = static_cast<std::uint16_t>(contract_.capture.depth);
        info.width = 32;
        info.fingerprint64 = contract_.fingerprints.fingerprint64;
    } else {
        DebugProtocol protocol(transport_);
        protocol.SetAutoReconnect(options.autoReconnect);
        if (options.syncCalibrate) {
            FireProgress(options, DebugAcqStage::SyncCalibrate, 0, "同步校准");
            if (!protocol.SyncCalibrate(error)) {
                error = "同步校准失败: " + error +
                        "。请先下载与当前契约匹配的 full 调试位流，并确认板载 FTDI "
                        "TX/RX 交叉连接到 dbg_rx/dbg_tx（17/18）且共地。";
                return false;
            }
            if (IsAborted(options)) { error = "Aborted"; return false; }
            FireProgress(options, DebugAcqStage::SyncCalibrate, 100, "校准完成");
        }
        FireProgress(options, DebugAcqStage::GetInfo, 0, "GET_INFO + 指纹校验");
        if (!protocol.GetInfo(info, error, options.commandTimeoutMs,
                              options.commandAttempts)) {
            error = "GET_INFO 失败: " + error;
            return false;
        }
        if (info.ipVersion != 1) {
            error = "协议版本不匹配：板载 " + std::to_string(info.ipVersion) +
                    "，当前契约要求 1";
            return false;
        }
        if (info.width != 32) {
            error = "采样宽度不匹配：板载 " + std::to_string(info.width) +
                    "，当前契约要求 32";
            return false;
        }
        if (info.depth != contract_.capture.depth) {
            error = "采样深度不匹配：板载 " + std::to_string(info.depth) +
                    "，当前契约要求 " + std::to_string(contract_.capture.depth);
            return false;
        }
        if (IsAborted(options)) { error = "Aborted"; return false; }
        if (options.checkFingerprint &&
            info.fingerprint64 != contract_.fingerprints.fingerprint64) {
            error = "指纹不一致：板载 0x" + FingerprintHex(info.fingerprint64) +
                    "，契约 0x" + FingerprintHex(contract_.fingerprints.fingerprint64);
            return false;
        }
        FireProgress(options, DebugAcqStage::GetInfo, 100,
                     "指纹 0x" + FingerprintHex(info.fingerprint64));
    }
    result_.fingerprint64 = info.fingerprint64;
    result_.depth = info.depth;
    if (IsAborted(options)) { error = "Aborted"; return false; }

    FireProgress(options, DebugAcqStage::MapTrigger, 0, "映射触发参数");
    std::uint32_t mask = 0, value = 0;
    std::uint8_t mode = 0;
    std::uint16_t count = 1;
    if (!TriggerParamsFromContract(contract_, mask, value, mode, count, error)) {
        error = "触发参数映射失败: " + error;
        return false;
    }
    FireProgress(options, DebugAcqStage::MapTrigger, 100,
                 "mode=" + std::to_string(mode) + " count=" + std::to_string(count));
    if (IsAborted(options)) { error = "Aborted"; return false; }

    DebugStatusInfo status;
    if (minimal) {
        if (contract_.capture.decimation != 1 || mode != 0 || count != 1) {
            error = "minimal UART 调试仅支持单次 mask_equal/none 触发和 decimation=1；"
                    "请选择 full 协议以使用扩展触发或抽取";
            return false;
        }
        MinimalDebugProtocol protocol(transport_);
        protocol.SetAutoReconnect(options.autoReconnect);

        FireProgress(options, DebugAcqStage::Configure, 0, "CONFIG mask/value");
        if (!protocol.Configure(mask, value, error, options.commandTimeoutMs,
                                options.commandAttempts)) {
            error = "CONFIG 失败: " + error;
            return false;
        }
        FireProgress(options, DebugAcqStage::Configure, 100, {});
        if (IsAborted(options)) { error = "Aborted"; return false; }

        FireProgress(options, DebugAcqStage::Arm, 0, "ARM 触发");
        if (!protocol.Arm(error, options.commandTimeoutMs, options.commandAttempts)) {
            error = "ARM 失败: " + error;
            return false;
        }
        FireProgress(options, DebugAcqStage::Arm, 100, {});
        if (IsAborted(options)) { error = "Aborted"; return false; }

        FireProgress(options, DebugAcqStage::PollStatus, 0, "等待采集完成（轮询 STATUS）");
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options.captureTimeoutMs);
        int tick = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (IsAborted(options)) { error = "Aborted"; return false; }
            if (!protocol.Status(status, error, options.commandTimeoutMs,
                                 options.commandAttempts)) {
                error = "STATUS 失败: " + error;
                return false;
            }
            if (status.flags & 0x40) break;
            const int elapsed = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - (deadline - std::chrono::milliseconds(options.captureTimeoutMs))).count());
            const int pct = std::min(99, std::max(0, elapsed * 100 / options.captureTimeoutMs));
            if ((tick++ % 10) == 0) {
                FireProgress(options, DebugAcqStage::PollStatus, pct, {});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(options.pollIntervalMs));
        }
        if (!(status.flags & 0x40)) {
            error = "采集超时（busy 未在 " +
                    std::to_string(options.captureTimeoutMs) + " ms 内完成）";
            return false;
        }
        FireProgress(options, DebugAcqStage::PollStatus, 100,
                     "采集完成，trigger_idx=" + std::to_string(status.triggerIndex));

        FireProgress(options, DebugAcqStage::ReadSamples, 0, "开始回读样本");
        if (!protocol.ReadCapture(0, info.depth, result_.samples, error,
                                  options.commandTimeoutMs, options.commandAttempts,
                                  [&](std::uint32_t got, std::uint32_t total) {
                                      if (!IsAborted(options)) {
                                          FireProgress(options, DebugAcqStage::ReadSamples,
                                                       static_cast<int>(got * 100 / total),
                                                       std::to_string(got) + "/" + std::to_string(total));
                                      }
                                  })) {
            if (IsAborted(options)) { error = "Aborted"; return false; }
            error = "读取采集数据失败: " + error;
            return false;
        }
        if (IsAborted(options)) { error = "Aborted"; return false; }
        FireProgress(options, DebugAcqStage::ReadSamples, 100,
                     "已回读 " + std::to_string(result_.samples.size()) + " 个样本");
    } else {
        DebugProtocol protocol(transport_);
        protocol.SetAutoReconnect(options.autoReconnect);

        FireProgress(options, DebugAcqStage::Configure, 0, "CONFIG mask/value/decimation/mode/count");
        if (!protocol.Configure(mask, value, contract_.capture.decimation, mode, count,
                                error, options.commandTimeoutMs, options.commandAttempts)) {
            error = "CONFIG 失败: " + error;
            return false;
        }
        FireProgress(options, DebugAcqStage::Configure, 100, {});
        if (IsAborted(options)) { error = "Aborted"; return false; }

        FireProgress(options, DebugAcqStage::Arm, 0, "ARM 触发");
        if (!protocol.Arm(error, options.commandTimeoutMs, options.commandAttempts)) {
            error = "ARM 失败: " + error;
            return false;
        }
        FireProgress(options, DebugAcqStage::Arm, 100, {});
        if (IsAborted(options)) { error = "Aborted"; return false; }

        FireProgress(options, DebugAcqStage::PollStatus, 0, "等待采集完成（轮询 STATUS）");
        const auto deadlineF = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(options.captureTimeoutMs);
        const auto start = std::chrono::steady_clock::now();
        int tick = 0;
        while (std::chrono::steady_clock::now() < deadlineF) {
            if (IsAborted(options)) { error = "Aborted"; return false; }
            if (!protocol.Status(status, error, options.commandTimeoutMs,
                                 options.commandAttempts)) {
                error = "STATUS 失败: " + error;
                return false;
            }
            if (status.flags & 0x40) break;
            const int elapsed = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());
            const int pct = std::min(99, std::max(0, elapsed * 100 / options.captureTimeoutMs));
            if ((tick++ % 10) == 0) {
                FireProgress(options, DebugAcqStage::PollStatus, pct, {});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(options.pollIntervalMs));
        }
        if (!(status.flags & 0x40)) {
            error = "采集超时（busy 未在 " +
                    std::to_string(options.captureTimeoutMs) + " ms 内完成）";
            return false;
        }
        FireProgress(options, DebugAcqStage::PollStatus, 100,
                     "采集完成，trigger_idx=" + std::to_string(status.triggerIndex));

        FireProgress(options, DebugAcqStage::ReadSamples, 0, "开始分块回读样本");
        if (!protocol.ReadCapture(0, info.depth, result_.samples, error,
                                  options.commandTimeoutMs, options.commandAttempts,
                                  [&](std::uint32_t got, std::uint32_t total) {
                                      if (!IsAborted(options)) {
                                          FireProgress(options, DebugAcqStage::ReadSamples,
                                                       static_cast<int>(got * 100 / total),
                                                       std::to_string(got) + "/" + std::to_string(total));
                                      }
                                  })) {
            if (IsAborted(options)) { error = "Aborted"; return false; }
            error = "读取采集数据失败: " + error;
            return false;
        }
        if (IsAborted(options)) { error = "Aborted"; return false; }
        FireProgress(options, DebugAcqStage::ReadSamples, 100,
                     "已回读 " + std::to_string(result_.samples.size()) + " 个样本");
    }
    result_.triggerIndex = status.triggerIndex;

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        error = "创建输出目录失败: " + ec.message();
        return false;
    }
    if (IsAborted(options)) { error = "Aborted"; return false; }

    result_.rawPath = outDir + "\\capture.raw";
    result_.vcdPath = outDir + "\\capture.vcd";
    FireProgress(options, DebugAcqStage::SaveRaw, 0, "写入 capture.raw");
    if (!SaveCaptureRaw(result_.rawPath, info.depth, info.width, 0, result_.samples,
                        error)) {
        return false;
    }
    FireProgress(options, DebugAcqStage::SaveRaw, 100,
                 "已保存 " + result_.rawPath);
    if (IsAborted(options)) { error = "Aborted"; return false; }

    FireProgress(options, DebugAcqStage::DecodeVcd, 0, "VCD 解码 + Probe 映射");
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
    FireProgress(options, DebugAcqStage::DecodeVcd, 100,
                 "已生成 " + result_.vcdPath);
    FireProgress(options, DebugAcqStage::Done, 100, "采集完成");
    return true;
}

bool DebugAcquisition::AcquireWithSession(const std::string& projectPath,
                                          const std::string& sessionId,
                                          const std::string& outDir, std::string& error,
                                          const DebugAcquisitionOptions& options)
{
    DebugSessionInfo session;
    DebugSessionService service;
    if (!service.Load(projectPath, sessionId, session, error)) return false;
    const auto RejectSession = [&](const std::string& reason) {
        error = reason;
        std::string transitionError;
        service.Transition(projectPath, sessionId, DebugSessionState::Failed,
                           reason, 1, transitionError);
        return false;
    };
    if (!session.protocol.empty() && session.protocol != contract_.transport.protocol) {
        return RejectSession("会话协议与当前契约不匹配：会话 " + session.protocol +
                             "，契约 " + contract_.transport.protocol);
    }
    if (session.clockHz != 0 && session.clockHz != contract_.sampleClock.frequencyHz) {
        return RejectSession("会话采样时钟与当前契约不匹配：会话 " +
                             std::to_string(session.clockHz) + " Hz，契约 " +
                             std::to_string(contract_.sampleClock.frequencyHz) + " Hz");
    }
    if (session.baud != 0 && session.baud != contract_.transport.baud) {
        return RejectSession("会话波特率与当前契约不匹配：会话 " + std::to_string(session.baud) +
                             "，契约 " + std::to_string(contract_.transport.baud));
    }
    if (session.captureDepth != 0 && session.captureDepth != contract_.capture.depth) {
        return RejectSession("会话采样深度与当前契约不匹配");
    }
    const bool ok = Acquire(outDir, error, options);
    DebugSessionState target = ok ? DebugSessionState::Captured : DebugSessionState::Failed;
    std::string reason = ok ? std::string("acquired") : error;
    if (!ok && error == "Aborted") {
        target = DebugSessionState::Failed;
    } else if (!ok && error.find("超时") != std::string::npos) {
        target = DebugSessionState::TimedOut;
    }
    std::string transitionError;
    if (!service.Transition(projectPath, sessionId, target, reason,
                            ok ? 0 : 1, transitionError)) {
        if (ok) { error = "会话状态迁移失败: " + transitionError; return false; }
    }
    return ok;
}

} // namespace debug
} // namespace sigflow
