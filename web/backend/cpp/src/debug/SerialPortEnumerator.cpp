#include "SerialPortEnumerator.h"

#include "eda-platform/SerialEnumerator.h"

namespace eda {
namespace debug {

std::vector<SerialPortInfo> EnumerateSerialPorts() {
    std::vector<SerialPortInfo> result;
    for (const auto& port : eda::platform::EnumerateSerialPorts()) {
        SerialPortInfo info;
        info.name = port.name;
        info.friendlyName = port.friendlyName;
        info.interfaceNumber = port.interfaceNumber;
        info.vendorId = port.vendorId;
        info.productId = port.productId;
        info.usbSerial = port.usbSerial;
        result.push_back(std::move(info));
    }
    return result;
}

}  // namespace debug
}  // namespace eda
