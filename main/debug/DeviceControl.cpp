#include "DeviceControl.h"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace sigflow {
namespace debug {

namespace {

std::mutex& LeaseMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::string>& Owners()
{
    static std::unordered_map<std::string, std::string> owners;
    return owners;
}

} // namespace

DeviceControlLease::~DeviceControlLease()
{
    Release();
}

DeviceControlLease::DeviceControlLease(DeviceControlLease&& other) noexcept
    : scope_(std::move(other.scope_)), owner_(std::move(other.owner_)), held_(other.held_)
{
    other.held_ = false;
}

DeviceControlLease& DeviceControlLease::operator=(DeviceControlLease&& other) noexcept
{
    if (this == &other) return *this;
    Release();
    scope_ = std::move(other.scope_);
    owner_ = std::move(other.owner_);
    held_ = other.held_;
    other.held_ = false;
    return *this;
}

bool DeviceControlLease::Acquire(const std::string& scope, const std::string& owner,
                                 DeviceControlLease& lease, std::string& error)
{
    error.clear();
    if (lease.IsHeld()) {
        error = "DEVICE_BUSY: output lease already owns " + lease.Scope();
        return false;
    }
    if (scope.empty()) { error = "DEVICE_BUSY: device control scope is empty"; return false; }
    if (owner.empty()) { error = "DEVICE_BUSY: device control owner is empty"; return false; }
    std::lock_guard<std::mutex> lock(LeaseMutex());
    const auto [it, inserted] = Owners().emplace(scope, owner);
    if (!inserted) {
        error = "DEVICE_BUSY: scope " + scope + " is owned by " + it->second;
        return false;
    }
    lease.scope_ = scope;
    lease.owner_ = owner;
    lease.held_ = true;
    return true;
}

void DeviceControlLease::Release()
{
    if (!held_) return;
    std::lock_guard<std::mutex> lock(LeaseMutex());
    const auto it = Owners().find(scope_);
    if (it != Owners().end() && it->second == owner_) Owners().erase(it);
    scope_.clear();
    owner_.clear();
    held_ = false;
}

} // namespace debug
} // namespace sigflow
