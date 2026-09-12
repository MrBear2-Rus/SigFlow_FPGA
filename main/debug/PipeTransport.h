#pragma once

#include "ITransport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sigflow {
namespace debug {

// 命名管道字节流传输（Windows，T-CI-01）。
// CreatePair 在同一进程内创建 host/device 两个对端，用于无硬件闭环测试；
// 与未来 SerialTransport 实现同一 ITransport 接口，可无缝替换。
class PipeTransport : public ITransport {
public:
    PipeTransport(std::string pipeName, bool server);
    ~PipeTransport() override;

    bool Open() override;
    void Close() override;
    bool IsOpen() const override { return hPipe_ != INVALID_HANDLE_VALUE; }

    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs) override;
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs) override;

    // 创建一对连通的命名管道端点（host 为客户端，device 为服务端）。
    static bool CreatePair(std::string pipeName,
                           std::unique_ptr<PipeTransport>& hostEnd,
                           std::unique_ptr<PipeTransport>& deviceEnd,
                           std::string& error);

private:
    bool OpenServer();
    bool OpenClient();
    static void ServerConnectLoop(HANDLE hPipe);

    std::string pipeName_;  // 完整路径 \\?\pipe\...（以 UTF-8 存储）
    bool server_ = false;
    HANDLE hPipe_ = INVALID_HANDLE_VALUE;
    std::thread connectThread_;
};

} // namespace debug
} // namespace sigflow
