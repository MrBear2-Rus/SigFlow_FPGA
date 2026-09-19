#pragma once
#include <string>
#include <vector>
namespace sigflow::platform {
struct SerialPortInfo {
    std::string name;
    std::string friendlyName;

    // --- USB 归属信息（Linux 从 sysfs 读取；Windows 分支暂不填）---
    // 双通道调试器（FT2232 家族、Tang Nano 全系板载 BL702 伪装 FT2232 等）
    // 会暴露两个串口：接口 0 = JTAG 通道，接口 1 = UART 通道。
    // 采集协议走 UART，因此必须能区分【接口号】——只看名字会选错通道。
    int         interfaceNumber = -1;   // bInterfaceNumber；-1 = 未知（非 USB 串口）
    std::string vendorId;               // "0403"
    std::string productId;              // "6010"
    std::string usbSerial;              // 设备序列号（同一块板子稳定）
};
std::vector<SerialPortInfo> EnumerateSerialPorts();
} // namespace sigflow::platform
