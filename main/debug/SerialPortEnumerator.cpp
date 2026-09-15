#include "SerialPortEnumerator.h"

#include "../platform/SerialEnumerator.h"

namespace sigflow {
namespace debug {

std::vector<SerialPortInfo> EnumerateSerialPorts()
{
    std::vector<SerialPortInfo> result;
    for (const auto& port : sigflow::platform::EnumerateSerialPorts()) {
        SerialPortInfo info;
        info.name = port.name;
        info.friendlyName = port.friendlyName;
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace debug
} // namespace sigflow
