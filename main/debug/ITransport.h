#pragma once

#include <cstddef>
#include <cstdint>

namespace sigflow {
namespace debug {

// 传输抽象（T-CI-01）：真实串口（SerialTransport，P3 实现）与
// PipeTransport（无硬件闭环）共用同一接口。
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool Open() = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    // 读入最多 size 字节，返回实际字节数。
    // 超时返回 0；对端关闭（断连）返回 0 且 IsOpen() 变为 false。
    virtual std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs) = 0;

    // 写 size 字节；超时或断连返回 false。
    virtual bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs) = 0;

    virtual void DiscardInput() {}
};

} // namespace debug
} // namespace sigflow
