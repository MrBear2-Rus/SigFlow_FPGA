#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sigflow::debug {

enum class MonitorBackpressureState {
    Healthy,
    Warning,
    Blocked,
};

struct MonitorStoragePolicy {
    std::uint64_t minimumFreeBytes = 0;
    std::uint32_t minimumFreePercent = 0;
    std::uint32_t warningFreePercent = 0;
};

struct MonitorStorageSample {
    std::string root;
    std::uint64_t capacityBytes = 0;
    std::uint64_t freeBytes = 0;
    std::uint64_t availableBytes = 0;
};

struct MonitorBackpressureResult {
    MonitorBackpressureState state = MonitorBackpressureState::Healthy;
    MonitorStorageSample sample;
    std::string code;
    std::string reason;
};

const char* ToString(MonitorBackpressureState state);

class MonitorBackpressureService {
public:
    // 只读取文件系统空间，不创建文件、不清理文件，也不触碰设备。
    static bool Sample(const std::string& outputRoot,
                       MonitorStorageSample& sample,
                       std::string& error);

    static MonitorBackpressureResult Evaluate(const MonitorStorageSample& sample,
                                              const MonitorStoragePolicy& policy);
};

using MonitorBackpressureProbe = std::function<MonitorBackpressureResult()>;

} // namespace sigflow::debug
