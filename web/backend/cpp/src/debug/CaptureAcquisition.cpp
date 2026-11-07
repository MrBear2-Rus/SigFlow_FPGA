#include "CaptureAcquisition.h"

#include "eda-core/JobPaths.h"
#include "eda-platform/Sha256.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace fs = std::filesystem;

namespace eda::debug {
namespace {

class DeadlineTransport final : public ITransport {
  public:
    DeadlineTransport(ITransport& inner, const CaptureAttemptContext& context)
        : inner_(inner), context_(context) {}

    bool Open() override {
        return !context_.cancelled.load() && std::chrono::steady_clock::now() < context_.deadline &&
               inner_.Open();
    }

    void Close() override { inner_.Close(); }

    bool IsOpen() const override {
        // Keep the physical connection state visible to auto-reconnect. A
        // cancelled operation must not look like a cable disconnect.
        return inner_.IsOpen();
    }

    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs) override {
        const int bounded = Bound(timeoutMs);
        if (bounded <= 0) {
            if (context_.cancelled.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return 0;
        }
        return inner_.Read(data, size, bounded);
    }

    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs) override {
        const int bounded = Bound(timeoutMs);
        return bounded > 0 && inner_.Write(data, size, bounded);
    }

    void DiscardInput() override {
        if (Active())
            inner_.DiscardInput();
    }

  private:
    bool Active() const {
        return !context_.cancelled.load() && std::chrono::steady_clock::now() < context_.deadline &&
               inner_.IsOpen();
    }

    int Bound(int requestedMs) const {
        if (context_.cancelled.load() || !inner_.IsOpen())
            return 0;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   context_.deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0)
            return 0;
        const int requested = requestedMs > 0 ? requestedMs : 1;
        return std::min(requested, static_cast<int>(std::min<std::int64_t>(
                                       remaining, std::numeric_limits<int>::max())));
    }

    ITransport& inner_;
    const CaptureAttemptContext& context_;
};

struct TransportStatsCapture {
    ITransport& transport;
    CaptureAcquisitionResult& result;
    ~TransportStatsCapture() {
        if (const auto* measured = dynamic_cast<const InstrumentedTransport*>(&transport))
            result.transportStats = measured->Stats();
    }
};

struct ProtocolStatsCapture {
    ProtocolStats& target;
    const ProtocolStats& source;
    ~ProtocolStatsCapture() { target = source; }
};

bool IsCancelled(const CaptureAttemptContext& context) {
    return context.cancelled.load();
}

int CommandTimeout(const CaptureAttemptContext& context, const CaptureAcquisitionOptions& options,
                   std::chrono::steady_clock::time_point deadline, std::string& error,
                   std::uint32_t attempts = 0) {
    if (IsCancelled(context)) {
        error = "Cancelled";
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
        error = "capture attempt deadline exceeded";
        return 0;
    }
    const auto boundedAttempts =
        std::max<std::uint32_t>(1, attempts == 0 ? options.commandAttempts : attempts);
    const auto perAttempt = std::max<std::int64_t>(1, remaining / boundedAttempts);
    return static_cast<int>(std::min<std::int64_t>(options.commandTimeoutMs, perAttempt));
}

bool IsIdentifier(const std::string& text) {
    if (text.empty())
        return false;
    const auto first = static_cast<unsigned char>(text.front());
    if (!std::isalpha(first) && first != '_')
        return false;
    return std::all_of(text.begin() + 1, text.end(), [](char raw) {
        const auto c = static_cast<unsigned char>(raw);
        return std::isalnum(c) || c == '_';
    });
}

bool ValidatePlan(const CaptureAcquisitionPlan& plan, const CaptureAcquisitionOptions& options,
                  std::string& error) {
    if (plan.depth == 0 || plan.depth > std::numeric_limits<std::uint16_t>::max()) {
        error = "capture depth must be in 1..65535";
        return false;
    }
    if (plan.pretriggerSamples != 0) {
        error = "linear capture does not support pretrigger samples";
        return false;
    }
    if (plan.sampleClockHz == 0 || plan.decimation == 0 ||
        plan.sampleClockHz > 1000000000000ULL * plan.decimation) {
        error = "capture requires a sample clock and a 1ps-resolvable decimation in 1..255";
        return false;
    }
    if (plan.triggerMode > 4 || plan.triggerCount == 0) {
        error = "capture trigger mode/count is invalid";
        return false;
    }
    if (plan.triggerMode == 2 && (plan.hsValidPath.empty() || plan.hsReadyPath.empty())) {
        error = "handshake trigger requires valid and ready paths";
        return false;
    }
    if (plan.probes.empty() || plan.probes.size() > 32) {
        error = "capture requires 1..32 probes";
        return false;
    }
    for (const auto& probe : plan.probes) {
        const auto dot = probe.path.rfind('.');
        const auto leaf = dot == std::string::npos ? probe.path : probe.path.substr(dot + 1);
        if (probe.id.empty() || probe.path.empty() || !IsIdentifier(leaf) || probe.width == 0 ||
            probe.width > 32 || probe.bitOffset >= 32 || probe.width > 32 - probe.bitOffset) {
            error = "capture probe id, path, width, or bit offset is invalid";
            return false;
        }
    }
    if (plan.protocol == CaptureWireProtocol::Minimal &&
        (plan.decimation != 1 || plan.triggerMode != 0 || plan.triggerCount != 1)) {
        error = "minimal protocol supports only mode 0, count 1, and decimation 1";
        return false;
    }
    if (options.pollIntervalMs == 0 || options.captureTimeoutMs == 0 ||
        options.commandTimeoutMs == 0 || options.commandAttempts == 0 ||
        options.commandAttempts > 10) {
        error = "capture polling and command timeout settings are invalid";
        return false;
    }
    return true;
}

void Progress(const CaptureAcquisitionOptions& options, CaptureAcquisitionStage stage, int percent,
              const std::string& message) {
    if (options.onProgress) {
        try {
            options.onProgress(stage, std::clamp(percent, 0, 100), message);
        } catch (...) {
        }
    }
}

std::string FingerprintHex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i) {
        result[static_cast<std::size_t>(i)] = digits[value & 0xFu];
        value >>= 4;
    }
    return result;
}

template <typename Protocol>
bool WaitAndRead(Protocol& protocol, const CaptureAcquisitionPlan& plan,
                 const CaptureAcquisitionOptions& options, const CaptureAttemptContext& context,
                 DebugStatusInfo& status, CaptureAcquisitionResult& result, std::string& error) {
    const auto waitStarted = std::chrono::steady_clock::now();
    const auto waitDeadline = std::min(
        context.deadline, waitStarted + std::chrono::milliseconds(options.captureTimeoutMs));
    result.timing.waitStartedAtUtc = CaptureNowUtc();
    Progress(options, CaptureAcquisitionStage::Waiting, 0, "等待采集完成");
    int tick = 0;
    while (std::chrono::steady_clock::now() < waitDeadline) {
        if (IsCancelled(context)) {
            error = "Cancelled";
            return false;
        }
        const int timeoutMs = CommandTimeout(context, options, waitDeadline, error);
        if (timeoutMs <= 0)
            return false;
        if (!protocol.Status(status, error, timeoutMs, static_cast<int>(options.commandAttempts))) {
            if (IsCancelled(context))
                error = "Cancelled";
            else if (context.RemainingBudgetMs() == 0)
                error = "capture attempt deadline exceeded";
            else
                error = "STATUS failed: " + error;
            return false;
        }
        if ((status.flags & 0x40u) != 0)
            break;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - waitStarted)
                                 .count();
        const auto percent =
            options.captureTimeoutMs == 0
                ? 0
                : static_cast<int>(std::min<std::uint64_t>(99, static_cast<std::uint64_t>(elapsed) *
                                                                   100 / options.captureTimeoutMs));
        if ((tick++ % 10) == 0)
            Progress(options, CaptureAcquisitionStage::Waiting, percent, {});
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   waitDeadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(
                std::min<std::int64_t>(options.pollIntervalMs, remaining)));
    }
    if ((status.flags & 0x40u) == 0) {
        error = std::chrono::steady_clock::now() >= context.deadline
                    ? "capture attempt deadline exceeded"
                    : "capture wait timed out";
        return false;
    }
    if ((status.flags & 0x10u) != 0) {
        error = "device reports capture done and busy at the same time";
        return false;
    }
    // A confirmed done and idle status is the only post-ARM point at which a
    // failed readback can safely be retried by issuing another ARM.
    result.safeToRearm = true;
    result.triggerIndex = status.triggerIndex;
    result.timing.captureReadyAtUtc = CaptureNowUtc();
    result.timing.waitMs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - waitStarted)
                                       .count());
    Progress(options, CaptureAcquisitionStage::Waiting, 100,
             "capture ready at sample " + std::to_string(status.triggerIndex));

    result.timing.readbackStartedAtUtc = CaptureNowUtc();
    const auto readStarted = std::chrono::steady_clock::now();
    Progress(options, CaptureAcquisitionStage::Reading, 0, "读取采集样本");
    const auto timeoutMs = CommandTimeout(context, options, context.deadline, error);
    if (timeoutMs <= 0)
        return false;
    const auto shouldContinue = [&]() {
        return !IsCancelled(context) && std::chrono::steady_clock::now() < context.deadline;
    };
    const auto progress = [&](std::uint32_t got, std::uint32_t total) {
        const int percent = total == 0 ? 0 : static_cast<int>(got * 100u / total);
        Progress(options, CaptureAcquisitionStage::Reading, percent,
                 std::to_string(got) + "/" + std::to_string(total));
    };
    if (!protocol.ReadCaptureCancellable(
            0, static_cast<std::uint16_t>(plan.depth), result.samples, error, timeoutMs,
            static_cast<int>(options.commandAttempts), progress, shouldContinue)) {
        if (IsCancelled(context))
            error = "Cancelled";
        else if (std::chrono::steady_clock::now() >= context.deadline)
            error = "capture attempt deadline exceeded";
        else
            error = "capture read failed: " + error;
        return false;
    }
    if (!shouldContinue()) {
        error = IsCancelled(context) ? "Cancelled" : "capture attempt deadline exceeded";
        return false;
    }
    result.timing.readbackCompletedAtUtc = CaptureNowUtc();
    result.timing.readbackMs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - readStarted)
                                       .count());
    Progress(options, CaptureAcquisitionStage::Reading, 100,
             "已读取 " + std::to_string(result.samples.size()) + " 个样本");
    return result.samples.size() == plan.depth;
}

template <typename Protocol>
bool RunProtocol(Protocol& protocol, const CaptureAcquisitionPlan& plan,
                 const fs::path& outputDirectory, const CaptureAttemptContext& context,
                 const CaptureAcquisitionOptions& options, CaptureAcquisitionResult& result,
                 std::string& error) {
    ProtocolStatsCapture protocolStatsCapture{result.protocolStats, protocol.Stats()};
    protocol.SetAutoReconnect(options.autoReconnect);

    DebugInfo info;
    if constexpr (std::is_same_v<Protocol, DebugProtocol>) {
        if (options.syncCalibrate) {
            Progress(options, CaptureAcquisitionStage::Synchronizing, 0, "同步校准");
            int timeoutMs = CommandTimeout(context, options, context.deadline, error);
            if (timeoutMs <= 0 ||
                !protocol.SyncCalibrate(error, timeoutMs,
                                        static_cast<int>(options.commandAttempts))) {
                if (error.empty())
                    error = "synchronization failed";
                return false;
            }
            if (IsCancelled(context)) {
                error = "Cancelled";
                return false;
            }
            Progress(options, CaptureAcquisitionStage::Synchronizing, 100, "校准完成");
        }
        Progress(options, CaptureAcquisitionStage::Identifying, 0, "读取设备信息");
        int timeoutMs = CommandTimeout(context, options, context.deadline, error);
        if (timeoutMs <= 0 ||
            !protocol.GetInfo(info, error, timeoutMs, static_cast<int>(options.commandAttempts))) {
            if (error.empty())
                error = "GET_INFO failed";
            return false;
        }
        if (info.ipVersion != proto::kIpVersion || info.width != 32 || info.depth != plan.depth) {
            error =
                "device protocol version, sample width, or depth does not match the capture plan";
            return false;
        }
        if ((info.flags & 0x10u) != 0) {
            error = "capture device is already busy";
            return false;
        }
        result.safeToRearm = true;
        if (plan.checkFingerprint && info.fingerprint64 != plan.expectedFingerprint64) {
            error = "device fingerprint 0x" + FingerprintHex(info.fingerprint64) +
                    " does not match expected fingerprint 0x" +
                    FingerprintHex(plan.expectedFingerprint64);
            return false;
        }
        result.fingerprintVerified = plan.checkFingerprint;
        Progress(options, CaptureAcquisitionStage::Identifying, 100,
                 "fingerprint 0x" + FingerprintHex(info.fingerprint64));
    } else {
        info.depth = static_cast<std::uint16_t>(plan.depth);
        info.width = 32;
        info.fingerprint64 = plan.expectedFingerprint64;
        result.fingerprintVerified = false;
        DebugStatusInfo initialStatus;
        const int timeoutMs = CommandTimeout(context, options, context.deadline, error);
        if (timeoutMs <= 0 || !protocol.Status(initialStatus, error, timeoutMs,
                                               static_cast<int>(options.commandAttempts))) {
            if (error.empty())
                error = "initial STATUS failed";
            return false;
        }
        if ((initialStatus.flags & 0x10u) != 0) {
            error = "capture device is already busy";
            return false;
        }
        result.safeToRearm = true;
    }
    result.depth = info.depth;
    result.width = info.width;
    result.fingerprint64 = info.fingerprint64;

    Progress(options, CaptureAcquisitionStage::Configuring, 0, "写入触发配置");
    int timeoutMs = CommandTimeout(context, options, context.deadline, error);
    if (timeoutMs <= 0)
        return false;
    if constexpr (std::is_same_v<Protocol, DebugProtocol>) {
        if (!protocol.Configure(plan.triggerMask, plan.triggerValue, plan.decimation,
                                plan.triggerMode, plan.triggerCount, error, timeoutMs,
                                static_cast<int>(options.commandAttempts))) {
            error = "CONFIG failed: " + error;
            return false;
        }
    } else if (!protocol.Configure(plan.triggerMask, plan.triggerValue, error, timeoutMs,
                                   static_cast<int>(options.commandAttempts))) {
        error = "CONFIG failed: " + error;
        return false;
    }
    Progress(options, CaptureAcquisitionStage::Configuring, 100, "配置完成");
    if (IsCancelled(context)) {
        error = "Cancelled";
        return false;
    }

    result.timing.armRequestedAtUtc = CaptureNowUtc();
    Progress(options, CaptureAcquisitionStage::Arming, 0, "ARM");
    timeoutMs = CommandTimeout(context, options, context.deadline, error, 1);
    if (timeoutMs <= 0)
        return false;
    // ARM is non-idempotent. Once the request can have reached the device,
    // leave retries disabled until STATUS confirms a completed idle capture.
    result.safeToRearm = false;
    if (!protocol.Arm(error, timeoutMs, 1)) {
        if (error.empty())
            error = "ARM failed";
        else
            error = "ARM failed: " + error;
        return false;
    }
    Progress(options, CaptureAcquisitionStage::Arming, 100, "已布防");

    DebugStatusInfo status;
    if (!WaitAndRead(protocol, plan, options, context, status, result, error))
        return false;

    const auto artifactStarted = std::chrono::steady_clock::now();
    Progress(options, CaptureAcquisitionStage::Saving, 0, "写入 capture.raw");
    result.rawPath = (outputDirectory / "capture.raw").string();
    if (!SaveCaptureRaw(result.rawPath, info.depth, info.width, 0, result.samples, error))
        return false;
    result.timing.artifactsCompletedAtUtc.clear();
    Progress(options, CaptureAcquisitionStage::Decoding, 0, "生成 VCD 与元数据");
    result.vcdPath = (outputDirectory / "capture.vcd").string();
    const bool triggerObserved = (status.flags & 0x20u) != 0;
    if (!DecodeCaptureToVcd(result.samples, info.depth, status.triggerIndex, plan.probes,
                            result.vcdPath, result.vcdMeta, error,
                            {plan.sampleClockHz, plan.decimation, triggerObserved})) {
        error = "VCD decode failed: " + error;
        return false;
    }
    result.metadataPath = (outputDirectory / "capture-meta.json").string();
    if (!SaveCaptureMetadata(result.metadataPath, result.vcdMeta, info.depth, status.triggerIndex,
                             error))
        return false;
    result.timing.artifactsCompletedAtUtc = CaptureNowUtc();
    result.timing.artifactMs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - artifactStarted)
                                       .count());
    Progress(options, CaptureAcquisitionStage::Decoding, 100, "采集产物已提交");
    Progress(options, CaptureAcquisitionStage::Completed, 100, "采集完成");
    return true;
}

void HashSamples(const std::vector<std::uint32_t>& samples, std::string& hash) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(samples.size() * 4);
    for (const auto sample : samples) {
        bytes.push_back(static_cast<std::uint8_t>(sample & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((sample >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((sample >> 16) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((sample >> 24) & 0xFF));
    }
    hash = platform::Sha256Hex(bytes.data(), bytes.size());
}

bool IsTimeoutError(const std::string& error) {
    return error.find("timeout") != std::string::npos ||
           error.find("timed out") != std::string::npos || error.find("超时") != std::string::npos;
}

}  // namespace

const char* ToString(CaptureAcquisitionStage stage) {
    switch (stage) {
    case CaptureAcquisitionStage::Synchronizing:
        return "synchronizing";
    case CaptureAcquisitionStage::Identifying:
        return "identifying";
    case CaptureAcquisitionStage::Configuring:
        return "configuring";
    case CaptureAcquisitionStage::Arming:
        return "arming";
    case CaptureAcquisitionStage::Waiting:
        return "waiting";
    case CaptureAcquisitionStage::Reading:
        return "reading";
    case CaptureAcquisitionStage::Saving:
        return "saving";
    case CaptureAcquisitionStage::Decoding:
        return "decoding";
    case CaptureAcquisitionStage::Completed:
        return "completed";
    }
    return "unknown";
}

bool AcquireCapture(ITransport& transport, const CaptureAcquisitionPlan& plan,
                    const fs::path& outputDirectory, const CaptureAttemptContext& context,
                    const CaptureAcquisitionOptions& options, CaptureAcquisitionResult& result,
                    std::string& error) {
    error.clear();
    result = CaptureAcquisitionResult{};
    TransportStatsCapture transportStatsCapture{transport, result};
    if (!ValidatePlan(plan, options, error))
        return false;
    if (IsCancelled(context)) {
        error = "Cancelled";
        return false;
    }
    if (std::chrono::steady_clock::now() >= context.deadline) {
        error = "capture attempt deadline exceeded";
        return false;
    }
    try {
        if (!fs::is_directory(outputDirectory)) {
            error = "capture output directory does not exist";
            return false;
        }
        web::CheckedPath(outputDirectory, ".", true);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    if (!transport.IsOpen()) {
        error = "capture transport is not open";
        return false;
    }

    DeadlineTransport boundedTransport(transport, context);
    try {
        if (plan.protocol == CaptureWireProtocol::Full) {
            DebugProtocol protocol(boundedTransport);
            return RunProtocol(protocol, plan, outputDirectory, context, options, result, error);
        }
        MinimalDebugProtocol protocol(boundedTransport);
        return RunProtocol(protocol, plan, outputDirectory, context, options, result, error);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

CaptureAttemptExecutor::CaptureAttemptExecutor(ITransport& transport, CaptureAcquisitionPlan plan,
                                               CaptureAcquisitionOptions options)
    : transport_(transport), plan_(std::move(plan)), options_(std::move(options)) {}

CaptureAttemptResult
CaptureAttemptExecutor::operator()(const CaptureAttemptContext& context) const {
    CaptureAttemptResult attempt;
    attempt.state = CaptureAttemptState::Running;
    attempt.attemptId = context.attemptId;
    attempt.window.sessionId = plan_.sessionId;
    attempt.window.captureId = context.attemptId;

    const fs::path runRoot =
        fs::absolute(context.plan.outputRoot).lexically_normal() / context.runId;
    const fs::path attemptRelative = fs::path("attempts") / context.attemptId;
    fs::path attemptDirectory;
    std::string error;
    CaptureAcquisitionResult result;
    try {
        if (context.attemptId.empty() || context.attemptId.size() > 160 ||
            context.attemptId.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_ ") !=
                std::string::npos ||
            context.attemptId.find(' ') != std::string::npos) {
            throw std::invalid_argument("capture attempt id is invalid");
        }
        web::CheckedPath(runRoot, ".", true);
        const auto attemptsDirectory = web::CheckedPath(runRoot, "attempts", false);
        fs::create_directories(attemptsDirectory);
        web::CheckedPath(runRoot, "attempts", true);
        attemptDirectory = web::CheckedPath(runRoot, attemptRelative, false);
        if (!fs::create_directory(attemptDirectory))
            throw std::runtime_error("capture attempt directory already exists");
    } catch (const std::exception& exception) {
        error = exception.what();
    }

    const auto started = std::chrono::steady_clock::now();
    const bool acquired = error.empty() && AcquireCapture(transport_, plan_, attemptDirectory,
                                                          context, options_, result, error);
    attempt.elapsedMs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());
    attempt.timing = result.timing;
    attempt.transportStats = result.transportStats;
    attempt.protocolStats = result.protocolStats;
    attempt.retryAllowed = result.safeToRearm;
    attempt.window.sampleCount = result.samples.size();
    attempt.window.triggerIndex = result.triggerIndex;
    attempt.window.sampleClockHz = plan_.sampleClockHz;
    attempt.window.decimation = plan_.decimation;
    attempt.window.timeDomain = result.vcdMeta.timeDomain;
    attempt.window.vcdTimescale = result.vcdMeta.timescale;
    attempt.window.triggerObserved = result.vcdMeta.triggerObserved;
    attempt.window.triggerTime = result.vcdMeta.triggerTime;
    attempt.window.identity = plan_.protocol == CaptureWireProtocol::Minimal
                                  ? "host_bound"
                                  : (result.fingerprintVerified ? "device_verified" : "unknown");
    if (!result.samples.empty())
        HashSamples(result.samples, attempt.window.dataHash);

    const auto relativePath = [&](const fs::path& file) {
        if (!file.empty() && fs::is_regular_file(file))
            return file.lexically_relative(runRoot).generic_string();
        return std::string();
    };
    attempt.window.rawPath = relativePath(result.rawPath);
    attempt.window.vcdPath = relativePath(result.vcdPath);
    attempt.window.metadataPath = relativePath(result.metadataPath);

    if (acquired) {
        attempt.state = CaptureAttemptState::Captured;
        attempt.window.completeness = "complete";
        attempt.window.classification =
            result.vcdMeta.triggerObserved ? "triggered" : "complete_untriggered";
        return attempt;
    }

    attempt.error = error.empty() ? "capture acquisition failed" : error;
    if (IsCancelled(context) || attempt.error == "Cancelled") {
        attempt.state = CaptureAttemptState::Cancelled;
        attempt.window.completeness = result.samples.empty() ? "unknown" : "partial";
    } else if (context.RemainingBudgetMs() == 0 || IsTimeoutError(attempt.error)) {
        attempt.state = CaptureAttemptState::TimedOut;
        attempt.window.completeness =
            attempt.window.sampleCount == plan_.depth ? "corrupt" : "partial";
    } else {
        attempt.state = CaptureAttemptState::Failed;
        attempt.window.completeness = attempt.window.sampleCount == 0 ? "unknown" : "partial";
    }
    const auto severity = attempt.state == CaptureAttemptState::Cancelled
                              ? DiagnosticSeverity::Warning
                              : DiagnosticSeverity::Error;
    const auto code =
        attempt.state == CaptureAttemptState::Cancelled
            ? "USER_CANCELLED"
            : (attempt.state == CaptureAttemptState::TimedOut ? "CAPTURE_TIMEOUT"
                                                              : "CAPTURE_ACQUISITION_FAILED");
    attempt.issues.push_back({code,
                              "acquiring",
                              severity,
                              attempt.error,
                              "检查设备、触发条件和调试链路；确认设备状态后再启动新 run",
                              {}});
    return attempt;
}

}  // namespace eda::debug
