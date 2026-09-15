#include "SerialTransport.h"

namespace sigflow {
namespace debug {

SerialTransport::SerialTransport(std::string portName, std::uint32_t baud)
    : port_(std::make_unique<sigflow::platform::SerialPort>(std::move(portName), baud))
{
}

SerialTransport::~SerialTransport() = default;

bool SerialTransport::Open()
{
    return port_->Open();
}

void SerialTransport::Close()
{
    port_->Close();
}

bool SerialTransport::IsOpen() const
{
    return port_->IsOpen();
}

std::size_t SerialTransport::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    return port_->Read(data, size, timeoutMs);
}

bool SerialTransport::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    return port_->Write(data, size, timeoutMs);
}

void SerialTransport::DiscardInput()
{
    port_->DiscardInput();
}

bool SerialTransport::SetBaudRate(std::uint32_t baud)
{
    return port_->SetBaudRate(baud);
}

std::string SerialTransport::PortName() const
{
    return port_->PortName();
}

bool SerialTransport::IsSupportedBaud(std::uint32_t baud)
{
    return sigflow::platform::SerialPort::IsSupportedBaud(baud);
}

} // namespace debug
} // namespace sigflow
