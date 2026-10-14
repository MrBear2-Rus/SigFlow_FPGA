#pragma once

#include <string>

namespace sigflow {
namespace debug {

// 进程内设备控制权。采集、烧录和自检应共享同一 scope；本轮先以稳定
// scope 串行化 TraceBridge 控制，后续可按设备枚举信息细分到具体目标。
class DeviceControlLease {
public:
    DeviceControlLease() = default;
    ~DeviceControlLease();

    DeviceControlLease(const DeviceControlLease&) = delete;
    DeviceControlLease& operator=(const DeviceControlLease&) = delete;
    DeviceControlLease(DeviceControlLease&& other) noexcept;
    DeviceControlLease& operator=(DeviceControlLease&& other) noexcept;

    static bool Acquire(const std::string& scope, const std::string& owner,
                        DeviceControlLease& lease, std::string& error);

    bool IsHeld() const { return held_; }
    const std::string& Scope() const { return scope_; }
    const std::string& Owner() const { return owner_; }
    void Release();

private:
    std::string scope_;
    std::string owner_;
    bool held_ = false;
};

} // namespace debug
} // namespace sigflow
