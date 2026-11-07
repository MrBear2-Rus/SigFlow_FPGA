#include "DeviceCommands.h"

#include "FtdiTransport.h"
#include "SerialPortEnumerator.h"
#include "TransportBackend.h"

#include <stdexcept>
#include <string>

namespace eda::debug {
namespace {

Json OptionalText(const std::string& value) {
    return value.empty() ? Json(nullptr) : Json(value);
}

Json SerialPorts() {
    Json result = Json::array();
    for (const auto& port : EnumerateSerialPorts()) {
        result.push_back({{"id", "serial:" + port.name},
                          {"path", port.name},
                          {"name", port.friendlyName},
                          {"interface_number",
                           port.interfaceNumber < 0 ? Json(nullptr) : Json(port.interfaceNumber)},
                          {"vendor_id", OptionalText(port.vendorId)},
                          {"product_id", OptionalText(port.productId)},
                          {"usb_serial", OptionalText(port.usbSerial)}});
    }
    return result;
}

Json FtdiProbe(const TransportBuildCapabilities& capabilities) {
    const bool compiled = capabilities.ftdi;
    return {{"compiled", compiled},
            {"vendor_id", "0403"},
            {"product_id", "6010"},
            {"device_present", compiled ? Json(FtdiTransport::IsDevicePresent()) : Json(nullptr)}};
}

}  // namespace

int DeviceCommand(int argc, char** argv, const std::function<void(const Json&)>& emit) {
    if (argc != 3 || std::string(argv[2]) != "list") {
        emit(Json{{"type", "diagnostic"},
                  {"severity", "error"},
                  {"code", "INVALID_ARGUMENT"},
                  {"message", "usage: sigflow-tool devices list"}});
        return 2;
    }

    try {
        const auto capabilities = GetTransportBuildCapabilities();
        Json data = {
            {"schema", "sigflow.devices.list.v1"},
            {"platform", capabilities.platform},
            {"capabilities", {{"serial", capabilities.serial}, {"ftdi", capabilities.ftdi}}},
            {"serial_ports", SerialPorts()},
            {"ftdi_probe", FtdiProbe(capabilities)}};
        emit(Json{{"type", "result"}, {"ok", true}, {"data", data}});
        return 0;
    } catch (const std::exception& error) {
        emit(Json{{"type", "diagnostic"},
                  {"severity", "error"},
                  {"code", "DEVICE_ENUMERATION_FAILED"},
                  {"message", error.what()}});
        return 4;
    }
}

}  // namespace eda::debug
