#pragma once

#include "CaptureDecoder.h"
#include "DebugContract.h"
#include "DebugProtocol.h"
#include "ITransport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// 端到端采集阶段（用于 GUI 进度驱动与日志 tag）。
enum class DebugAcqStage {
    Idle = 0,
    SyncCalibrate,      // 1 同步校准
    GetInfo,            // 2 GET_INFO 与指纹校验
    MapTrigger,         // 3 触发参数映射
    Configure,          // 4 CONFIG
    Arm,                // 5 ARM
    PollStatus,         // 6 轮询 STATUS（等待触发或超时）
    ReadSamples,        // 7 分块回读采集样本
    SaveRaw,            // 8 保存 capture.raw
    DecodeVcd,          // 9 VCD 解码
    Done,               // 10 完成
};

// 阶段进度回调：stage（当前阶段）、percentInStage（阶段内 0..100，未知时 -1）、message（描述）。
using DebugAcqProgressCb = std::function<void(DebugAcqStage stage, int percentInStage,
                                              const std::string& message)>;

struct DebugAcquisitionOptions {
    bool syncCalibrate = true;
    bool checkFingerprint = true;
    bool autoReconnect = true;
    int pollIntervalMs = 20;
    int captureTimeoutMs = 5000;
    int commandTimeoutMs = 1000;
    int commandAttempts = 3;

    // 可选项：取消标志位（true 时在下一个阻塞检查点立即返回 false + "Aborted"）。
    std::atomic<bool>* aborted = nullptr;
    // 可选项：阶段进度回调（可能在 worker 线程被调用，调用方需自行 marshal 到 UI 线程）。
    DebugAcqProgressCb onProgress;
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

    // 采集到 outDir（capture.raw / capture.vcd）。线程安全的取消通过 options.aborted 注入。
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

    // 辅助：阶段名 / 总进度 0..100 映射（GUI 显示用）。
    static const char* StageName(DebugAcqStage stage);
    static int StageOrder(DebugAcqStage stage);  // 0..10，用于总进度百分比估算

private:
    static bool IsAborted(const DebugAcquisitionOptions& options);
    void FireProgress(const DebugAcquisitionOptions& options, DebugAcqStage stage,
                      int percentInStage, const std::string& message) const;

    ITransport& transport_;
    const DebugContract& contract_;
    DebugAcquisitionResult result_;
};

} // namespace debug
} // namespace sigflow
