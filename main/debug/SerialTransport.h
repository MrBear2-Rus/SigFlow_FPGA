#pragma once

#include "ITransport.h"

#include <cstdint>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sigflow {
namespace debug {

// Windows COM 串口传输（T-P3-01），实现 ITransport 接口：
// 8N1、无流控、重叠 I/O 读写超时；与 SerialPortEnumerator 配套。
class SerialTransport : public ITransport {
public:
    SerialTransport(std::string portName, std::uint32_t baud = 921600);
    ~SerialTransport() override;

    bool Open() override;
    void Close() override;
    bool IsOpen() const override { return hCom_ != INVALID_HANDLE_VALUE; }

    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs) override;
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs) override;
    void DiscardInput() override;

    bool SetBaudRate(std::uint32_t baud);
    std::string PortName() const { return portName_; }
    static bool IsSupportedBaud(std::uint32_t baud);

private:
    std::string portName_;  // "COM3"（打开时补 \\.\ 前缀）
    std::uint32_t baud_;
    HANDLE hCom_ = INVALID_HANDLE_VALUE;
};

} // namespace debug
} // namespace sigflow
