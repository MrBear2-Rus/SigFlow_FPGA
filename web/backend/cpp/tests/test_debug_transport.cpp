#define _XOPEN_SOURCE 600

#include "debug/FtdiTransport.h"
#include "debug/DeviceCommands.h"
#include "debug/SerialPortEnumerator.h"
#include "debug/SerialTransport.h"
#include "debug/TransportBackend.h"
#include "eda-platform/SerialPort.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace {

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void CheckFtdiStatusFiltering() {
    bool inFrame = false;
    auto first = eda::debug::FtdiTransport::FilterStreamBytes({0x11, 0x00, 0x11, 0x01}, inFrame);
    Check(first == std::vector<std::uint8_t>({0x00, 0x11, 0x01}),
          "preserve frame bytes and discard outside status byte");
    Check(inFrame, "preserve frame state across reads");
    auto second = eda::debug::FtdiTransport::FilterStreamBytes({0x00, 0x11}, inFrame);
    Check(second == std::vector<std::uint8_t>({0x00}), "discard status after closing delimiter");
    Check(!inFrame, "clear frame state after closing delimiter");
}

void CheckSerialValidation() {
    Check(eda::platform::SerialPort::IsSupportedBaud(921600), "common baud supported");
    Check(!eda::platform::SerialPort::IsSupportedBaud(250000), "unsupported baud rejected");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("sigflow-serial-not-a-device-" + std::to_string(stamp));
    {
        std::ofstream file(path);
        Check(static_cast<bool>(file), "create serial test file");
    }
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};

    eda::platform::SerialPort unsupported(path.string(), 250000);
    Check(!unsupported.Open() && !unsupported.IsOpen(), "unsupported baud cannot open a port");
    eda::platform::SerialPort regularFile(path.string(), 921600);
    Check(!regularFile.Open() && !regularFile.IsOpen(), "regular files are not serial devices");

    eda::debug::TransportOpenResult result;
    std::string error;
    Check(!eda::debug::OpenDebugTransport({path.string(), 250000, false, false}, result, error),
          "transport selector rejects unsupported serial baud");
    Check(!result.structuredDiagnostics.empty() &&
              result.structuredDiagnostics.front().code ==
                  eda::debug::TransportDiagnosticCode::InvalidConfiguration,
          "unsupported baud has configuration diagnostic");
}

void CheckSerialEnumerationShape() {
    for (const auto& port : eda::debug::EnumerateSerialPorts()) {
        Check(!port.name.empty(), "enumerated serial path is nonempty");
        Check(!port.friendlyName.empty(), "enumerated serial label is nonempty");
    }
}

void CheckDevicesCommand() {
    char executable[] = "sigflow-tool";
    char command[] = "devices";
    char subcommand[] = "list";
    char* args[] = {executable, command, subcommand};
    std::vector<eda::Json> events;
    const int status = eda::debug::DeviceCommand(
        3, args, [&](const eda::Json& event) { events.push_back(event); });
    Check(status == 0 && events.size() == 1, "devices command success response");
    const auto& event = events.front();
    Check(event.at("type") == "result" && event.at("ok") == true, "devices command JSONL envelope");
    const auto& data = event.at("data");
    Check(data.at("schema") == "sigflow.devices.list.v1" && data.at("platform") == "linux",
          "devices command schema and platform");
    Check(data.at("capabilities").at("serial") == true && data.at("serial_ports").is_array(),
          "devices command reports serial capability and ports");
    Check(data.at("ftdi_probe").at("compiled") == data.at("capabilities").at("ftdi"),
          "devices command distinguishes FTDI build capability");

    char invalid[] = "show";
    char* invalidArgs[] = {executable, command, invalid};
    events.clear();
    Check(eda::debug::DeviceCommand(
              3, invalidArgs, [&](const eda::Json& response) { events.push_back(response); }) == 2,
          "devices rejects unsupported subcommand");
    Check(events.size() == 1 && events.front().at("code") == "INVALID_ARGUMENT",
          "devices usage diagnostic");
}

void CheckPosixSerialByteStream() {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    Check(master >= 0, "create pseudo terminal");
    struct Cleanup {
        int fd;
        ~Cleanup() {
            if (fd >= 0)
                ::close(fd);
        }
    } cleanup{master};
    Check(::grantpt(master) == 0 && ::unlockpt(master) == 0, "prepare pseudo terminal");
    const char* slaveName = ::ptsname(master);
    Check(slaveName != nullptr, "resolve pseudo terminal slave");

    eda::debug::SerialTransport serial(slaveName, 921600);
    Check(serial.Open() && serial.IsOpen(), "open pseudo terminal serial endpoint");

    const std::uint8_t incoming[] = {0x00, 0x11, 0x7e, 0xff};
    Check(::write(master, incoming, sizeof(incoming)) == sizeof(incoming), "write to serial");
    std::uint8_t received[sizeof(incoming)] = {};
    Check(serial.Read(received, sizeof(received), 1000) == sizeof(received), "read serial bytes");
    Check(std::equal(std::begin(incoming), std::end(incoming), std::begin(received)),
          "serial read preserves bytes");

    const std::uint8_t outgoing[] = {0x42, 0x00, 0xa5};
    Check(serial.Write(outgoing, sizeof(outgoing), 1000), "write serial bytes");
    pollfd readable{master, POLLIN, 0};
    Check(::poll(&readable, 1, 1000) == 1, "wait for pseudo terminal data");
    std::uint8_t response[sizeof(outgoing)] = {};
    Check(::read(master, response, sizeof(response)) == sizeof(response), "read from serial peer");
    Check(std::equal(std::begin(outgoing), std::end(outgoing), std::begin(response)),
          "serial write preserves bytes");

    ::close(cleanup.fd);
    cleanup.fd = -1;
    std::uint8_t disconnected = 0;
    Check(serial.Read(&disconnected, 1, 1000) == 0 && !serial.IsOpen(),
          "peer disconnect closes serial endpoint");
}

}  // namespace

int main() {
    try {
        CheckFtdiStatusFiltering();
        CheckSerialValidation();
        CheckSerialEnumerationShape();
        CheckDevicesCommand();
        CheckPosixSerialByteStream();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
