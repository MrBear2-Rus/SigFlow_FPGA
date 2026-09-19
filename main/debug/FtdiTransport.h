#pragma once
#include "ITransport.h"
#include <cstdint>
#include <memory>
#include <string>

namespace sigflow {
namespace debug {

// 用 libftdi 直接驱动开发板的双通道调试器（FT2232 家族 / Tang Nano 板载 BL702 等），
// **绕过内核 ftdi_sio 与 tty**。
//
// 为什么必须绕过（均为本机实测）：
//   1) 这类芯片只是"模仿" FT2232；Tang Nano 9K 上同一个 PING 帧，
//      libftdi 直连能收到合法 Pong（00 08 01 02 01 01 01 15 42 00），
//      而 /dev/ttyUSB1 路径完全没有应答 —— 说明 ftdi_sio 走的 UART 厂商命令
//      没有被模拟层真正生效。
//   2) libftdi 打开设备会 detach ftdi_sio，烧录后调试串口的 tty 常常直接消失。
//
// 另外：libftdi 不剥离 FTDI 状态字节（内核驱动会），实测字节流里会混入 0x11。
// 因此读路径做过滤：**只丢弃"帧外"（0x00 定界之间以外）的 0x11**，帧内字节一律保留，
// 这样既清掉注入的状态字节，又不会误伤 COBS 帧里合法的 0x11。
class FtdiTransport : public ITransport {
public:
    // interfaceIndex: 0=通道A(JTAG)，1=通道B(调试 UART)。
    FtdiTransport(std::uint16_t vendorId = 0x0403, std::uint16_t productId = 0x6010,
                  int interfaceIndex = 1, std::uint32_t baud = 921600);
    ~FtdiTransport() override;

    bool Open() override;
    void Close() override;
    bool IsOpen() const override;
    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs) override;
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs) override;
    void DiscardInput() override;

    // 本构建是否带 libftdi 支持；以及目标设备是否真的存在。
    static bool IsSupported();
    static bool IsDevicePresent(std::uint16_t vendorId = 0x0403,
                                std::uint16_t productId = 0x6010);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace debug
} // namespace sigflow
