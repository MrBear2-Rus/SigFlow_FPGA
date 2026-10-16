#pragma once

#include "DebugContract.h"
#include "JtagDebugProtocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sigflow::debug {

struct JtagCaptureOptions {
    int pollIntervalMs = 20;
    int captureTimeoutMs = 5000;
    std::uint32_t expectedIdentity = 0;
    bool checkIdentity = false;
    std::atomic<bool>* aborted = nullptr;
    std::function<void(int percent, const std::string& message)> onProgress;
};

struct JtagCaptureResult {
    std::vector<std::uint32_t> samples;
    std::uint32_t identity = 0;
    std::uint16_t width = 0;
    std::uint16_t depth = 0;
    std::uint16_t triggerIndex = 0;
    bool triggerObserved = false;
    std::uint32_t statusWord = 0;
};

// One bounded capture over the versioned JTAG register protocol.  The adapter
// does not own a TAP/USB handle and never retries ARM; the exchange callback
// remains responsible for transport timeout and cancellation semantics.
class JtagCaptureAdapter {
public:
    JtagCaptureAdapter(JtagDebugProtocol& protocol, const DebugContract& contract);

    bool Capture(JtagCaptureResult& result, std::string& error,
                 const JtagCaptureOptions& options = {});

private:
    bool IsAborted(const JtagCaptureOptions& options) const;
    void Progress(const JtagCaptureOptions& options, int percent,
                  const std::string& message) const;
    bool ReadStatusUntilDone(std::uint32_t& status, std::string& error,
                             const JtagCaptureOptions& options);

    JtagDebugProtocol& protocol_;
    const DebugContract& contract_;
};

} // namespace sigflow::debug
