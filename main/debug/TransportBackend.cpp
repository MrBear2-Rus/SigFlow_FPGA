#include "TransportBackend.h"

#include "FtdiTransport.h"
#include "SerialTransport.h"
#include "TransportMetrics.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace sigflow {
namespace debug {

namespace {

void AddDiagnostic(TransportOpenResult& result,
                   TransportDiagnosticCode code,
                   const std::string& backend,
                   const std::string& text,
                   bool retryable)
{
    result.diagnostics.push_back(text);
    result.structuredDiagnostics.push_back({code, backend, text, retryable});
}

bool TryFtdi(const TransportOpenOptions& options, TransportOpenResult& result)
{
    constexpr std::uint16_t kVendorId = 0x0403;
    constexpr std::uint16_t kProductId = 0x6010;
    result.attempted.push_back("ftdi:0403:6010:chB");
    if (!FtdiTransport::IsSupported()) {
        AddDiagnostic(result, TransportDiagnosticCode::BackendUnavailable,
                      "ftdi:0403:6010:chB",
                      "FTDI backend is unavailable in this build (SIGFLOW_HAVE_LIBFTDI=0).", false);
        return false;
    }
    if (!FtdiTransport::IsDevicePresent(kVendorId, kProductId)) {
        AddDiagnostic(result, TransportDiagnosticCode::DeviceMissing,
                      "ftdi:0403:6010:chB",
                      "FTDI device 0403:6010 was not enumerated.", true);
        return false;
    }
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto ftdi = std::make_unique<FtdiTransport>(kVendorId, kProductId, 1, options.baud);
        if (ftdi->Open()) {
            result.backendId = "ftdi:0403:6010:chB";
            result.transport = std::make_unique<InstrumentedTransport>(std::move(ftdi));
            AddDiagnostic(result, TransportDiagnosticCode::Opened,
                          result.backendId, "FTDI channel B opened.", false);
            return true;
        }
        if (attempt < 4) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    AddDiagnostic(result, TransportDiagnosticCode::OpenFailed,
                  "ftdi:0403:6010:chB",
                  "FTDI device was present but opening channel B failed after 5 attempts.", true);
    return false;
}

bool TrySerial(const TransportOpenOptions& options, TransportOpenResult& result)
{
    result.attempted.push_back("serial:" + options.serialPort);
    if (options.serialPort.empty()) {
        AddDiagnostic(result, TransportDiagnosticCode::InvalidConfiguration,
                      "serial", "Serial backend was requested without a port name.", false);
        return false;
    }
    auto serial = std::make_unique<SerialTransport>(options.serialPort, options.baud);
    if (!serial->Open()) {
        AddDiagnostic(result, TransportDiagnosticCode::OpenFailed,
                      "serial:" + options.serialPort,
                      "Serial port " + options.serialPort + " failed to open.", true);
        return false;
    }
    result.backendId = "serial:" + options.serialPort;
    result.transport = std::make_unique<InstrumentedTransport>(std::move(serial));
    AddDiagnostic(result, TransportDiagnosticCode::Opened,
                  result.backendId, "Serial backend opened.", false);
    return true;
}

} // namespace

const char* ToString(TransportDiagnosticCode code)
{
    switch (code) {
        case TransportDiagnosticCode::InvalidConfiguration: return "INVALID_CONFIGURATION";
        case TransportDiagnosticCode::BackendUnavailable: return "BACKEND_UNAVAILABLE";
        case TransportDiagnosticCode::DeviceMissing: return "DEVICE_MISSING";
        case TransportDiagnosticCode::OpenFailed: return "OPEN_FAILED";
        case TransportDiagnosticCode::NoBackend: return "NO_BACKEND";
        case TransportDiagnosticCode::Opened: return "OPENED";
    }
    return "OPEN_FAILED";
}

bool OpenDebugTransport(const TransportOpenOptions& options,
                        TransportOpenResult& result,
                        std::string& error)
{
    error.clear();
    result = TransportOpenResult();
    if (options.baud == 0) {
        result.structuredDiagnostics.push_back({TransportDiagnosticCode::InvalidConfiguration,
                                                 "", "debug transport baud must be greater than zero", false});
        error = "debug transport baud must be greater than zero";
        return false;
    }

    if (options.preferFtdi && TryFtdi(options, result)) return true;
    if (options.preferFtdi && !options.allowSerialFallback) {
        AddDiagnostic(result, TransportDiagnosticCode::NoBackend,
                      "", "preferred FTDI backend could not be opened", false);
        error = "preferred FTDI backend could not be opened";
        return false;
    }
    if ((!options.preferFtdi || options.allowSerialFallback) && TrySerial(options, result)) return true;

    const std::string lastReason = result.diagnostics.empty() ? std::string() : result.diagnostics.back();
    error = "no debug transport could be opened";
    AddDiagnostic(result, TransportDiagnosticCode::NoBackend, "", error, false);
    if (!lastReason.empty()) error += ": " + lastReason;
    return false;
}

} // namespace debug
} // namespace sigflow
