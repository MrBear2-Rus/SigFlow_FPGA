#include "DiagnosticCatalog.h"

#include <algorithm>

namespace sigflow {
namespace debug {
namespace {

using D = DiagnosticCodeDescriptor;

const std::vector<D> kCatalog = {
    {"CAPTURE_RUN_INVALID", "validating", DiagnosticSeverity::Blocking,
     "capture run plan is invalid", "Correct the run identifier, output path, or timeout budget."},
    {"CAPTURE_RUN_NO_EXECUTOR", "validating", DiagnosticSeverity::Blocking,
     "capture run has no attempt executor", "Provide a capture callback before starting the run."},
    {"CAPTURE_RUN_TIMEOUT", "acquiring", DiagnosticSeverity::Error,
     "the capture run exceeded its time budget", "Inspect device state and retry after the transport is healthy."},
    {"CAPTURE_RUN_INCOMPLETE", "acquiring", DiagnosticSeverity::Error,
     "the run completed without enough complete windows", "Review attempt diagnostics and capture readiness."},
    {"CAPTURE_RUN_BLOCKED", "acquiring", DiagnosticSeverity::Blocking,
     "the run stopped after its failure limit", "Resolve the repeated failure before issuing another ARM."},
    {"CAPTURE_FAILURE_LIMIT", "acquiring", DiagnosticSeverity::Error,
     "consecutive capture failures reached the configured limit", "Inspect the first failed attempt and transport evidence."},
    {"CAPTURE_RETRY_UNSAFE", "acquiring", DiagnosticSeverity::Blocking,
     "the executor did not prove that another ARM is safe", "Re-establish device state before retrying."},
    {"USER_CANCELLED", "acquiring", DiagnosticSeverity::Info,
     "the operator cancelled the run", "No automatic retry is required; inspect partial artifacts if needed."},
    {"RUN_PERSIST_FAILED", "persisting", DiagnosticSeverity::Error,
     "capture artifacts could not be persisted", "Check the output directory and available storage."},
    {"CONTRACT_INVALID", "validating", DiagnosticSeverity::Blocking,
     "the debug contract failed validation", "Fix the contract fields and validate it again."},
    {"CONTRACT_NOT_READY", "validating", DiagnosticSeverity::Blocking,
     "the debug contract is not ready for acquisition", "Complete contract and target selection before capture."},
    {"CONTRACT_MISSING", "validating", DiagnosticSeverity::Blocking,
     "no debug contract was supplied", "Load or create a debug contract before capture."},
    {"DEVICE_MISSING", "validating", DiagnosticSeverity::Blocking,
     "the selected device is not present", "Reconnect the board and refresh device enumeration."},
    {"DEVICE_UNAVAILABLE", "validating", DiagnosticSeverity::Error,
     "the selected device cannot be opened", "Close competing tools and run transport self-test."},
    {"DEVICE_BUSY", "acquiring", DiagnosticSeverity::Error,
     "the device is owned by another operation", "Release the control lease and retry."},
    {"DEVICE_REENUMERATED", "acquiring", DiagnosticSeverity::Warning,
     "the device identity changed during capture", "Refresh enumeration and verify the target identity."},
    {"DEVICE_RESET_REQUIRED", "recovering", DiagnosticSeverity::Warning,
     "the transport requires a device reset", "Reset or power-cycle the board before retrying."},
    {"CONTROL_LEASE_BUSY", "validating", DiagnosticSeverity::Error,
     "another session owns the device control lease", "Close the other session or wait for its lease to expire."},
    {"TRANSPORT_BUSY", "acquiring", DiagnosticSeverity::Error,
     "the transport is busy", "Wait for the active transaction to finish before retrying."},
    {"TRANSPORT_PORT_MISSING", "validating", DiagnosticSeverity::Blocking,
     "the configured transport port is missing", "Refresh ports and select the connected board port."},
    {"TRANSPORT_BAUD_INVALID", "validating", DiagnosticSeverity::Blocking,
     "the configured transport baud is invalid", "Use a supported baud rate for the selected backend."},
    {"SERIAL_DEVICE_MISSING", "validating", DiagnosticSeverity::Blocking,
     "the serial device is not enumerated", "Reconnect the board and refresh serial devices."},
    {"FTDI_DEVICE_MISSING", "validating", DiagnosticSeverity::Blocking,
     "the FTDI device is not enumerated", "Check the FTDI cable, driver, and device identity."},
    {"BACKEND_UNAVAILABLE", "validating", DiagnosticSeverity::Blocking,
     "the requested transport backend is unavailable", "Install or enable the backend required by the contract."},
    {"OPEN_FAILED", "acquiring", DiagnosticSeverity::Error,
     "the transport could not be opened", "Inspect the underlying transport error and retry."},
    {"HANDSHAKE_FAILED", "acquiring", DiagnosticSeverity::Error,
     "the board handshake failed", "Verify firmware compatibility and transport settings."},
    {"SYNC_LOST", "acquiring", DiagnosticSeverity::Error,
     "capture synchronization was lost", "Inspect framing and device reset evidence before retrying."},
    {"TIMING_MARGIN_LOW", "analysing", DiagnosticSeverity::Warning,
     "the measured timing margin is low", "Review the timing report and reduce probe impact if needed."},
    {"TIMING_VIOLATION", "analysing", DiagnosticSeverity::Error,
     "the timing report contains a violation", "Resolve the reported violation before trusting the capture."},
    {"DUPLICATE_PROBE_ID", "validating", DiagnosticSeverity::Blocking,
     "two probes use the same stable identifier", "Assign unique probe IDs and rebuild the contract."},
    {"PROBE_BUDGET_EXCEEDED", "validating", DiagnosticSeverity::Blocking,
     "instrumentation exceeds the configured probe budget", "Reduce probe width or select a larger capture budget."},
    {"PROBE_TOO_WIDE", "validating", DiagnosticSeverity::Blocking,
     "a probe exceeds the supported width", "Split the probe or choose a supported width."},
    {"BUILD_FINGERPRINT_MISMATCH", "validating", DiagnosticSeverity::Blocking,
     "the bitstream fingerprint does not match the contract", "Rebuild or load the bitstream paired with the contract."},
    {"BUILD_FINGERPRINT_UNAVAILABLE", "validating", DiagnosticSeverity::Warning,
     "the bitstream fingerprint could not be verified", "Generate build metadata before relying on capture identity."},
    {"BITSTREAM_MISSING", "validating", DiagnosticSeverity::Blocking,
     "the selected bitstream is missing", "Select an existing bitstream and rerun preflight."},
    {"BITSTREAM_STALE", "validating", DiagnosticSeverity::Blocking,
     "the selected bitstream is stale", "Rebuild the bitstream from the current design."},
    {"SESSION_NOT_READY", "validating", DiagnosticSeverity::Blocking,
     "the debug session is not ready", "Complete session setup and rerun preflight."},
    {"STORAGE_LOW_BLOCKED", "persisting", DiagnosticSeverity::Blocking,
     "available storage is below the blocking threshold", "Free storage before starting another capture."},
    {"STORAGE_LOW_WARNING", "persisting", DiagnosticSeverity::Warning,
     "available storage is below the warning threshold", "Plan storage cleanup before long captures."},
};

} // namespace

const std::vector<DiagnosticCodeDescriptor>& KnownDiagnosticCodes()
{
    return kCatalog;
}

const DiagnosticCodeDescriptor* FindDiagnosticCode(const std::string& code)
{
    const auto it = std::find_if(kCatalog.begin(), kCatalog.end(),
        [&code](const DiagnosticCodeDescriptor& descriptor) {
            return descriptor.code == code;
        });
    return it == kCatalog.end() ? nullptr : &*it;
}

} // namespace debug
} // namespace sigflow
