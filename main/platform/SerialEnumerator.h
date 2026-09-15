#pragma once
#include <string>
#include <vector>
namespace sigflow::platform {
struct SerialPortInfo { std::string name; std::string friendlyName; };
std::vector<SerialPortInfo> EnumerateSerialPorts();
} // namespace sigflow::platform
