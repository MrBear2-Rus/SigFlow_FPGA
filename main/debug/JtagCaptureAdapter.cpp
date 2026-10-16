#include "JtagCaptureAdapter.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace sigflow::debug {
namespace {

bool ParseMaskValue(const DebugContract& contract, std::uint32_t& mask,
                    std::uint32_t& value, std::string& error)
{
    mask = 0;
    value = 0;
    if (contract.trigger.kind.empty() || contract.trigger.kind == "none") return true;
    if (contract.trigger.kind != "mask_equal") {
        error = "JTAG capture currently supports only none/mask_equal triggers";
        return false;
    }
    if (!ParseHexU32(contract.trigger.mask, mask) ||
        !ParseHexU32(contract.trigger.value, value)) {
        error = "JTAG mask_equal trigger requires hexadecimal mask and value";
        return false;
    }
    return true;
}

} // namespace

JtagCaptureAdapter::JtagCaptureAdapter(JtagDebugProtocol& protocol,
                                       const DebugContract& contract)
    : protocol_(protocol), contract_(contract)
{
}

bool JtagCaptureAdapter::IsAborted(const JtagCaptureOptions& options) const
{
    return options.aborted != nullptr && options.aborted->load();
}

void JtagCaptureAdapter::Progress(const JtagCaptureOptions& options, int percent,
                                  const std::string& message) const
{
    if (options.onProgress) {
        try {
            options.onProgress(percent, message);
        } catch (...) {
            // Progress callbacks are observers and cannot fail acquisition.
        }
    }
}

bool JtagCaptureAdapter::ReadStatusUntilDone(std::uint32_t& status,
                                             std::string& error,
                                             const JtagCaptureOptions& options)
{
    if (options.captureTimeoutMs <= 0 || options.pollIntervalMs < 0) {
        error = "JTAG capture timeout/poll interval is invalid";
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(options.captureTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (IsAborted(options)) {
            error = "Aborted";
            return false;
        }
        if (!protocol_.ReadStatus(status, error)) {
            error = "JTAG STATUS failed: " + error;
            return false;
        }
        const bool busy = (status & 0x1u) != 0;
        const bool done = (status & 0x2u) != 0;
        if (done && !busy) return true;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        Progress(options, static_cast<int>(std::min<std::int64_t>(99,
            elapsed * 100 / std::max(1, options.captureTimeoutMs))),
            "waiting for JTAG capture");
        if (options.pollIntervalMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(options.pollIntervalMs));
    }
    error = "JTAG capture timeout";
    return false;
}

bool JtagCaptureAdapter::Capture(JtagCaptureResult& result, std::string& error,
                                 const JtagCaptureOptions& options)
{
    result = JtagCaptureResult();
    error.clear();
    if (options.captureTimeoutMs <= 0 || options.pollIntervalMs < 0) {
        error = "JTAG capture timeout/poll interval is invalid";
        return false;
    }
    if (contract_.capture.pretriggerSamples != 0) {
        error = "JTAG capture does not support pretrigger samples in protocol jtag-1";
        return false;
    }
    if (contract_.capture.decimation < 1 || contract_.capture.decimation > 255) {
        error = "JTAG capture decimation must be in 1..255";
        return false;
    }
    std::uint32_t mask = 0, value = 0;
    if (!ParseMaskValue(contract_, mask, value, error)) return false;
    if (IsAborted(options)) { error = "Aborted"; return false; }

    Progress(options, 0, "JTAG identity");
    if (!protocol_.Identity(result.identity, error)) return false;
    if (options.checkIdentity && options.expectedIdentity != 0 &&
        result.identity != options.expectedIdentity) {
        error = "JTAG identity mismatch: expected " +
                std::to_string(options.expectedIdentity) + ", got " +
                std::to_string(result.identity);
        return false;
    }
    std::uint16_t width = 0, depth = 0;
    if (!protocol_.GetInfo(width, depth, error)) return false;
    if (width != 32) {
        error = "JTAG capture width mismatch: expected 32, got " + std::to_string(width);
        return false;
    }
    if (contract_.capture.depth != 0 && depth != contract_.capture.depth) {
        error = "JTAG capture depth mismatch: expected " +
                std::to_string(contract_.capture.depth) + ", got " +
                std::to_string(depth);
        return false;
    }
    result.width = width;
    result.depth = depth;
    if (IsAborted(options)) { error = "Aborted"; return false; }

    Progress(options, 15, "JTAG configure");
    if (!protocol_.ConfigureTriggerMask(mask, error) ||
        !protocol_.ConfigureTriggerValue(value, error) ||
        !protocol_.ConfigureDecimation(static_cast<std::uint8_t>(contract_.capture.decimation), error)) {
        return false;
    }
    if (IsAborted(options)) { error = "Aborted"; return false; }

    // ARM is intentionally issued once. A lost acknowledgement is not proof
    // that the core remained unarmed, so the caller must decide whether to
    // start a fresh capture after inspecting the failure.
    Progress(options, 25, "JTAG arm");
    if (!protocol_.Arm(error)) return false;
    if (IsAborted(options)) { error = "Aborted"; return false; }

    Progress(options, 30, "JTAG status");
    if (!ReadStatusUntilDone(result.statusWord, error, options)) return false;
    result.triggerObserved = (result.statusWord & 0x4u) != 0;
    result.triggerIndex = static_cast<std::uint16_t>((result.statusWord >> 16) & 0xFFFFu);
    if (IsAborted(options)) { error = "Aborted"; return false; }

    result.samples.reserve(result.depth);
    for (std::uint32_t address = 0; address < result.depth; ++address) {
        if (IsAborted(options)) { error = "Aborted"; return false; }
        std::uint32_t sample = 0;
        if (!protocol_.ReadCapture(address, sample, error)) {
            error = "JTAG capture read failed at address " + std::to_string(address) +
                    ": " + error;
            return false;
        }
        result.samples.push_back(sample);
        if ((address & 0x3Fu) == 0 || address + 1 == result.depth) {
            Progress(options, 30 + static_cast<int>((address + 1) * 70 / result.depth),
                     "JTAG read " + std::to_string(address + 1) + "/" +
                     std::to_string(result.depth));
        }
    }
    Progress(options, 100, "JTAG capture complete");
    return true;
}

} // namespace sigflow::debug
