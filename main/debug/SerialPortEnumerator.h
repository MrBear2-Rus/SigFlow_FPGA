#pragma once

#include <string>
#include <vector>

namespace sigflow {
namespace debug {

struct SerialPortInfo {
    std::string name;         // "COM3"
    std::string friendlyName; // "USB-SERIAL CH340 (COM3)"；取不到时等于 name

    // --- USB 归属信息（由 platform 层从 sysfs 读取；Windows 上为 -1/空）---
    // 双通道调试器（FT2232 家族、Tang Nano 板载 BL702 等）暴露两个串口：
    // 接口 0 = JTAG，接口 1 = UART。采集协议走 UART，必须按接口号区分通道。
    int         interfaceNumber = -1;   // bInterfaceNumber；-1 = 未知
    std::string vendorId;               // "0403"
    std::string productId;              // "6010"
    std::string usbSerial;              // 设备序列号
};

// 枚举本机串口（SetupAPI 友好名称 + 注册表 SERIALCOMM 兜底）。
std::vector<SerialPortInfo> EnumerateSerialPorts();

} // namespace debug
} // namespace sigflow
