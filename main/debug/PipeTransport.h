#pragma once

#include "ITransport.h"
#include "../platform/LocalPipe.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sigflow {
namespace debug {

// 本地字节流传输（T-CI-01）。
// CreatePair 在同一进程内创建 host/device 两个对端，用于无硬件闭环测试；
// 平台差异由 sigflow::platform::LocalPipe 承担（Win 命名管道 / POSIX AF_UNIX）。
class PipeTransport : public ITransport {
public:
    PipeTransport(std::string pipeName, bool server);
    ~PipeTransport() override;

    bool Open() override;
    void Close() override;
    bool IsOpen() const override;

    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs) override;
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs) override;

    // 创建一对连通的端点（host 为客户端，device 为服务端）。
    static bool CreatePair(std::string pipeName,
                           std::unique_ptr<PipeTransport>& hostEnd,
                           std::unique_ptr<PipeTransport>& deviceEnd,
                           std::string& error);

private:
    explicit PipeTransport(std::unique_ptr<sigflow::platform::LocalPipe> pipe);

    std::unique_ptr<sigflow::platform::LocalPipe> pipe_;
};

} // namespace debug
} // namespace sigflow
