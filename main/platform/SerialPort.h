#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
namespace sigflow::platform {
class SerialPort {
public:
    SerialPort(std::string portName, std::uint32_t baud = 921600);
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    bool Open();
    void Close();
    bool IsOpen() const;
    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs);
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs);
    void DiscardInput();
    bool SetBaudRate(std::uint32_t baud);
    const std::string& PortName() const;
    static bool IsSupportedBaud(std::uint32_t baud);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sigflow::platform
