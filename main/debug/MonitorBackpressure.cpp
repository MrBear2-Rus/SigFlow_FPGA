#include "MonitorBackpressure.h"

#include <algorithm>
#include <filesystem>

namespace sigflow::debug {

const char* ToString(MonitorBackpressureState state)
{
    switch (state) {
        case MonitorBackpressureState::Healthy: return "Healthy";
        case MonitorBackpressureState::Warning: return "Warning";
        case MonitorBackpressureState::Blocked: return "Blocked";
    }
    return "Blocked";
}

bool MonitorBackpressureService::Sample(const std::string& outputRoot,
                                        MonitorStorageSample& sample,
                                        std::string& error)
{
    error.clear();
    sample = MonitorStorageSample();
    if (outputRoot.empty()) {
        error = "monitor storage outputRoot is empty";
        return false;
    }
    std::error_code ec;
    const auto info = std::filesystem::space(outputRoot, ec);
    if (ec) {
        error = "unable to sample monitor storage: " + ec.message();
        return false;
    }
    sample.root = outputRoot;
    sample.capacityBytes = info.capacity;
    sample.freeBytes = info.free;
    sample.availableBytes = info.available;
    return true;
}

MonitorBackpressureResult MonitorBackpressureService::Evaluate(
    const MonitorStorageSample& sample, const MonitorStoragePolicy& policy)
{
    MonitorBackpressureResult result;
    result.sample = sample;
    if (sample.capacityBytes == 0) {
        result.state = MonitorBackpressureState::Blocked;
        result.code = "STORAGE_CAPACITY_UNKNOWN";
        result.reason = "storage capacity is unavailable";
        return result;
    }

    const std::uint64_t availablePercent =
        sample.availableBytes > (UINT64_MAX / 100)
            ? 100
            : (sample.availableBytes * 100) / sample.capacityBytes;
    if ((policy.minimumFreeBytes != 0 && sample.availableBytes < policy.minimumFreeBytes) ||
        (policy.minimumFreePercent != 0 && availablePercent < policy.minimumFreePercent)) {
        result.state = MonitorBackpressureState::Blocked;
        result.code = "STORAGE_LOW_BLOCKED";
        result.reason = "available storage is below the capture safety threshold";
        return result;
    }
    if (policy.warningFreePercent != 0 && availablePercent < policy.warningFreePercent) {
        result.state = MonitorBackpressureState::Warning;
        result.code = "STORAGE_LOW_WARNING";
        result.reason = "available storage is approaching the capture safety threshold";
        return result;
    }
    result.state = MonitorBackpressureState::Healthy;
    result.code = "STORAGE_OK";
    result.reason = "available storage is within policy";
    return result;
}

} // namespace sigflow::debug
