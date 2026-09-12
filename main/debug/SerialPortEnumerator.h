#pragma once

#include <string>
#include <vector>

namespace sigflow {
namespace debug {

struct SerialPortInfo {
    std::string name;         // "COM3"
    std::string friendlyName; // "USB-SERIAL CH340 (COM3)"；取不到时等于 name
};

// 枚举本机串口（SetupAPI 友好名称 + 注册表 SERIALCOMM 兜底）。
std::vector<SerialPortInfo> EnumerateSerialPorts();

} // namespace debug
} // namespace sigflow
