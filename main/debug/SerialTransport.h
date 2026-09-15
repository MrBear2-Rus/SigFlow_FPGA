#pragma once

#include "ITransport.h"
#include "../platform/SerialPort.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sigflow {
namespace debug {

// 串口传输（T-P3-01），实现 ITransport 接口：
// 8N1、无流控、读写超时；平台差异由 sigflow::platform::SerialPort 承担。
class SerialTransport : public ITransport {
public:
    SerialTransport(std::string portName, std::uint32_t baud = 921600);
    ~SerialTransport() override;

    bool Open() override;
    void Close() override;
    bool IsOpen() const override;

    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs) override;
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs) override;
    void DiscardInput() override;

    bool SetBaudRate(std::uint32_t baud);
    std::string PortName() const;
    static bool IsSupportedBaud(std::uint32_t baud);

private:
    std::unique_ptr<sigflow::platform::SerialPort> port_;
};

} // namespace debug
} // namespace sigflow
