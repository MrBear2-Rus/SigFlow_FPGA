#include "TraceBridgePreflight.h"

#include "DebugThresholds.h"
#include "FtdiTransport.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace sigflow {
namespace debug {

namespace {

int PhaseRank(PreflightPhase phase)
{
    return static_cast<int>(phase);
}

DiagnosticIssue Issue(const std::string& code, const std::string& stage,
                      DiagnosticSeverity severity, const std::string& observed,
                      const std::string& action)
{
    DiagnosticIssue issue;
    issue.code = code;
    issue.stage = stage;
    issue.severity = severity;
    issue.observed = observed;
    issue.action = action;
    return issue;
}

PreflightCheckResult Passed(const std::string& id, PreflightPhase phase,
                            bool requiresDevice, const std::string& summary)
{
    PreflightCheckResult result;
    result.id = id;
    result.phase = phase;
    result.state = PreflightState::Passed;
    result.requiresDevice = requiresDevice;
    result.offlineSafe = !requiresDevice;
    result.summary = summary;
    return result;
}

PreflightCheckResult Warning(const std::string& id, PreflightPhase phase,
                             const std::string& code, const std::string& observed,
                             const std::string& action)
{
    PreflightCheckResult result;
    result.id = id;
    result.phase = phase;
    result.state = PreflightState::Warning;
    result.summary = observed;
    result.issues.push_back(Issue(code, "preflight", DiagnosticSeverity::Warning,
                                  observed, action));
    return result;
}

PreflightCheckResult Failed(const std::string& id, PreflightPhase phase,
                            const std::string& code, const std::string& observed,
                            const std::string& action)
{
    PreflightCheckResult result;
    result.id = id;
    result.phase = phase;
    result.state = PreflightState::Failed;
    result.summary = observed;
    result.issues.push_back(Issue(code, "preflight", DiagnosticSeverity::Blocking,
                                   observed, action));
    return result;
}

PreflightCheckResult CheckContract(const PreflightInput& input)
{
    const std::string id = "contract.validate";
    if (!input.contract) {
        return Failed(id, PreflightPhase::Static, "CONTRACT_MISSING",
                      "no TraceBridge debug contract was supplied",
                      "select or generate a debug-contract.json before continuing");
    }
    std::string error;
    if (!input.contract->Validate(error)) {
        return Failed(id, PreflightPhase::Static, "CONTRACT_INVALID", error,
                      "repair the contract fields and run preflight again");
    }
    return Passed(id, PreflightPhase::Static, false,
                  "debug contract schema, probes, capture and transport fields are valid");
}

PreflightCheckResult CheckProbeBudget(const PreflightInput& input)
{
    const std::string id = "contract.probe_budget";
    if (!input.contract) {
        PreflightCheckResult result;
        result.id = id;
        result.phase = PreflightPhase::Capability;
        result.state = PreflightState::Skipped;
        result.summary = "skipped because the debug contract is missing";
        result.issues.push_back(Issue("CHECK_SKIPPED", "preflight",
                                      DiagnosticSeverity::Info, result.summary,
                                      "provide a valid debug contract"));
        return result;
    }

    unsigned totalBits = 0;
    std::set<std::string> ids;
    for (const DebugProbe& probe : input.contract->probes) {
        if (!ids.insert(probe.id).second) {
            return Failed(id, PreflightPhase::Capability, "DUPLICATE_PROBE_ID",
                          "duplicate probe id: " + probe.id,
                          "give every probe a stable unique id");
        }
        totalBits += probe.width;
    }
    if (totalBits > DebugDefaults::kProbeTotalBits) {
        return Failed(id, PreflightPhase::Capability, "PROBE_BUDGET_EXCEEDED",
                      "probe width uses " + std::to_string(totalBits) +
                          " bits; the current core supports " +
                          std::to_string(DebugDefaults::kProbeTotalBits),
                      "remove or defer probes, then regenerate the overlay");
    }
    return Passed(id, PreflightPhase::Capability, false,
                  "probe budget is " + std::to_string(totalBits) + "/" +
                      std::to_string(DebugDefaults::kProbeTotalBits) + " bits");
}

PreflightCheckResult CheckTransport(const PreflightInput& input)
{
    const std::string id = "transport.configuration";
    if (input.transport) {
        if (input.transport->baud == 0) {
            return Failed(id, PreflightPhase::Capability, "TRANSPORT_BAUD_INVALID",
                          "transport baud must be greater than zero",
                          "select a supported baud rate");
        }
        if (input.transport->preferFtdi && !GetTransportBuildCapabilities().ftdi) {
            const std::string observed =
                "FTDI backend is unavailable in this build; serial fallback is " +
                std::string(input.transport->allowSerialFallback ? "enabled" : "disabled");
            if (!input.transport->allowSerialFallback) {
                return Failed(id, PreflightPhase::Capability, "BACKEND_UNAVAILABLE",
                              observed,
                              "install/enable libftdi or select a serial backend explicitly");
            }
            return Warning(id, PreflightPhase::Capability, "BACKEND_UNAVAILABLE",
                           observed,
                           "select serial explicitly or use a build with libftdi when FTDI is required");
        }
        if (input.transport->serialPort.empty() && !input.transport->preferFtdi) {
            return Failed(id, PreflightPhase::Capability, "TRANSPORT_PORT_MISSING",
                          "serial-only transport has no port name",
                          "select the target serial port or enable FTDI discovery");
        }
        return Passed(id, PreflightPhase::Capability, false,
                      "host transport options are internally consistent");
    }
    if (input.contract && input.contract->transport.baud == 0) {
        return Failed(id, PreflightPhase::Capability, "CONTRACT_BAUD_MISSING",
                      "contract transport baud is missing",
                      "apply contract defaults or provide an explicit baud rate");
    }
    return Passed(id, PreflightPhase::Capability, false,
                  "transport configuration is sourced from the validated contract");
}

std::string NormalizePortName(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (value.rfind("\\\\.\\", 0) == 0) value.erase(0, 4);
    return value;
}

PreflightCheckResult CheckDevice(const PreflightInput& input)
{
    const std::string id = "device.enumerate";
    if (!input.transport) {
        PreflightCheckResult result;
        result.id = id;
        result.phase = PreflightPhase::Device;
        result.state = PreflightState::Skipped;
        result.summary = "device check skipped because no runtime transport target was supplied";
        result.issues.push_back(Issue("DEVICE_TARGET_MISSING", "preflight",
                                      DiagnosticSeverity::Info, result.summary,
                                      "select a serial/FTDI target before starting capture"));
        return result;
    }

    const bool ftdiPresent = input.isFtdiPresent
                                 ? input.isFtdiPresent()
                                 : FtdiTransport::IsDevicePresent();
    if (input.transport->preferFtdi && ftdiPresent) {
        return Passed(id, PreflightPhase::Device, true,
                      "FTDI debug device is enumerated; no port has been opened");
    }

    const auto ports = input.enumerateSerialPorts
                           ? input.enumerateSerialPorts()
                           : EnumerateSerialPorts();
    const std::string requested = NormalizePortName(input.transport->serialPort);
    const auto found = std::find_if(ports.begin(), ports.end(), [&](const SerialPortInfo& port) {
        return !requested.empty() && NormalizePortName(port.name) == requested;
    });
    if (found != ports.end()) {
        return Passed(id, PreflightPhase::Device, true,
                      "serial target " + found->name + " is enumerated; no port has been opened");
    }

    if (input.transport->preferFtdi && !input.transport->allowSerialFallback) {
        return Failed(id, PreflightPhase::Device, "FTDI_DEVICE_MISSING",
                      "preferred FTDI debug device was not enumerated",
                      "connect the FTDI/JTAG adapter or enable serial fallback");
    }
    if (requested.empty()) {
        return Failed(id, PreflightPhase::Device, "DEVICE_TARGET_MISSING",
                      "no serial target was selected and no FTDI debug device was enumerated",
                      "select the target serial port or connect the FTDI/JTAG adapter");
    }
    return Failed(id, PreflightPhase::Device, "SERIAL_DEVICE_MISSING",
                  "serial target " + input.transport->serialPort +
                      " was not enumerated; no port has been opened",
                  "refresh the device list, reconnect the board, or choose another target");
}

} // namespace

const char* ToString(PreflightPhase phase)
{
    switch (phase) {
        case PreflightPhase::Static: return "static";
        case PreflightPhase::Capability: return "capability";
        case PreflightPhase::Build: return "build";
        case PreflightPhase::Device: return "device";
    }
    return "static";
}

const char* ToString(PreflightState state)
{
    switch (state) {
        case PreflightState::Passed: return "passed";
        case PreflightState::Warning: return "warning";
        case PreflightState::Failed: return "failed";
        case PreflightState::Skipped: return "skipped";
        case PreflightState::Cancelled: return "cancelled";
    }
    return "failed";
}

bool PreflightRegistry::Register(PreflightCheckDescriptor descriptor, std::string& error)
{
    error.clear();
    if (descriptor.id.empty()) {
        error = "preflight check id must not be empty";
        return false;
    }
    if (!descriptor.callback) {
        error = "preflight check callback is missing: " + descriptor.id;
        return false;
    }
    const auto duplicate = std::find_if(checks_.begin(), checks_.end(),
                                        [&](const PreflightCheckDescriptor& item) {
                                            return item.id == descriptor.id;
                                        });
    if (duplicate != checks_.end()) {
        error = "duplicate preflight check id: " + descriptor.id;
        return false;
    }
    descriptor.offlineSafe = descriptor.offlineSafe && !descriptor.requiresDevice;
    checks_.push_back(std::move(descriptor));
    return true;
}

bool PreflightRegistry::Run(const PreflightInput& input, PreflightReport& report,
                            std::string& error) const
{
    error.clear();
    report = PreflightReport();
    std::vector<const PreflightCheckDescriptor*> ordered;
    ordered.reserve(checks_.size());
    for (const auto& descriptor : checks_) ordered.push_back(&descriptor);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (PhaseRank(left->phase) != PhaseRank(right->phase))
            return PhaseRank(left->phase) < PhaseRank(right->phase);
        return left->id < right->id;
    });

    for (const PreflightCheckDescriptor* descriptor : ordered) {
        if (input.isCancelled && input.isCancelled()) {
            report.cancelled = true;
            PreflightCheckResult cancelled;
            cancelled.id = descriptor->id;
            cancelled.phase = descriptor->phase;
            cancelled.state = PreflightState::Cancelled;
            cancelled.requiresDevice = descriptor->requiresDevice;
            cancelled.offlineSafe = descriptor->offlineSafe;
            cancelled.summary = "preflight cancelled before this check ran";
            cancelled.issues.push_back(Issue("PREFLIGHT_CANCELLED", "preflight",
                                             DiagnosticSeverity::Warning,
                                             cancelled.summary,
                                             "restart preflight when the current operation is idle"));
            report.checks.push_back(std::move(cancelled));
            continue;
        }
        if (input.offlineOnly && !descriptor->offlineSafe) {
            PreflightCheckResult skipped;
            skipped.id = descriptor->id;
            skipped.phase = descriptor->phase;
            skipped.state = PreflightState::Skipped;
            skipped.requiresDevice = descriptor->requiresDevice;
            skipped.offlineSafe = descriptor->offlineSafe;
            skipped.summary = "device check skipped in offline-only mode";
            skipped.issues.push_back(Issue("DEVICE_CHECK_SKIPPED", "preflight",
                                           DiagnosticSeverity::Info, skipped.summary,
                                           "connect the target and rerun device preflight"));
            report.checks.push_back(std::move(skipped));
            continue;
        }

        PreflightCheckResult result = descriptor->callback(input);
        result.id = descriptor->id;
        result.phase = descriptor->phase;
        result.requiresDevice = descriptor->requiresDevice;
        result.offlineSafe = descriptor->offlineSafe;
        report.checks.push_back(std::move(result));
    }

    for (const auto& check : report.checks) {
        report.issues.insert(report.issues.end(), check.issues.begin(), check.issues.end());
        if (check.state == PreflightState::Failed || check.state == PreflightState::Cancelled)
            report.ready = false;
    }
    if (!report.cancelled && std::none_of(report.checks.begin(), report.checks.end(),
                                          [](const auto& check) {
                                              return check.state == PreflightState::Failed ||
                                                     check.state == PreflightState::Cancelled;
                                          })) {
        report.ready = true;
    }
    if (report.cancelled) {
        error = "TraceBridge preflight was cancelled";
        return false;
    }
    if (!report.ready) {
        error = "TraceBridge preflight found blocking checks";
        return false;
    }
    return true;
}

PreflightRegistry MakeTraceBridgePreflightRegistry()
{
    PreflightRegistry registry;
    std::string ignored;
    registry.Register({"contract.validate", PreflightPhase::Static, false, true,
                       CheckContract}, ignored);
    registry.Register({"contract.probe_budget", PreflightPhase::Capability, false, true,
                       CheckProbeBudget}, ignored);
    registry.Register({"transport.configuration", PreflightPhase::Capability, false, true,
                       CheckTransport}, ignored);
    registry.Register({"device.enumerate", PreflightPhase::Device, true, false,
                       CheckDevice}, ignored);
    return registry;
}

} // namespace debug
} // namespace sigflow
