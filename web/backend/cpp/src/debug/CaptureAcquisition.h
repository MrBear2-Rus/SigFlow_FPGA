#pragma once

#include "CaptureDecoder.h"
#include "CaptureRunService.h"
#include "ITransport.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace eda::debug {

enum class CaptureWireProtocol { Full, Minimal };

enum class CaptureAcquisitionStage {
    Synchronizing,
    Identifying,
    Configuring,
    Arming,
    Waiting,
    Reading,
    Saving,
    Decoding,
    Completed,
};

struct CaptureAcquisitionPlan {
    std::string sessionId;
    CaptureWireProtocol protocol = CaptureWireProtocol::Full;
    std::uint32_t depth = 0;
    std::uint32_t pretriggerSamples = 0;
    std::uint64_t sampleClockHz = 0;
    std::uint8_t decimation = 1;
    std::uint64_t expectedFingerprint64 = 0;
    bool checkFingerprint = true;
    // Already normalized from the old DebugContract trigger mapping. The wire
    // protocol receives mask/value/mode/count; paths retain handshake intent.
    std::uint32_t triggerMask = 0;
    std::uint32_t triggerValue = 0;
    std::uint8_t triggerMode = 0;
    std::uint16_t triggerCount = 1;
    std::string hsValidPath;
    std::string hsReadyPath;
    std::vector<CaptureProbe> probes;
};

using CaptureAcquisitionProgress =
    std::function<void(CaptureAcquisitionStage stage, int percent, const std::string& message)>;

struct CaptureAcquisitionOptions {
    bool syncCalibrate = true;
    bool autoReconnect = true;
    std::uint32_t pollIntervalMs = 20;
    std::uint32_t captureTimeoutMs = 5000;
    std::uint32_t commandTimeoutMs = 1000;
    std::uint32_t commandAttempts = 3;
    CaptureAcquisitionProgress onProgress;
};

struct CaptureAcquisitionResult {
    std::vector<std::uint32_t> samples;
    std::uint16_t triggerIndex = 0;
    std::uint64_t fingerprint64 = 0;
    std::uint16_t depth = 0;
    std::uint8_t width = 0;
    bool fingerprintVerified = false;
    bool safeToRearm = false;
    CaptureVcdMeta vcdMeta;
    std::string rawPath;
    std::string vcdPath;
    std::string metadataPath;
    CaptureAttemptTiming timing;
    TransportStats transportStats;
    ProtocolStats protocolStats;
};

bool AcquireCapture(ITransport& transport, const CaptureAcquisitionPlan& plan,
                    const std::filesystem::path& outputDirectory,
                    const CaptureAttemptContext& context, const CaptureAcquisitionOptions& options,
                    CaptureAcquisitionResult& result, std::string& error);

// Binds an already-open leased transport and validated hardware plan to the
// run scheduler. The callback writes each attempt below the active run folder.
class CaptureAttemptExecutor {
  public:
    CaptureAttemptExecutor(ITransport& transport, CaptureAcquisitionPlan plan,
                           CaptureAcquisitionOptions options = {});

    CaptureAttemptResult operator()(const CaptureAttemptContext& context) const;

  private:
    ITransport& transport_;
    CaptureAcquisitionPlan plan_;
    CaptureAcquisitionOptions options_;
};

const char* ToString(CaptureAcquisitionStage stage);

}  // namespace eda::debug
