#include "TraceBridgeRecovery.h"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <utility>

namespace sigflow::debug {
namespace {

bool ContainsAny(const std::set<std::string>& codes,
                 std::initializer_list<const char*> candidates)
{
    for (const char* candidate : candidates) {
        if (codes.find(candidate) != codes.end()) return true;
    }
    return false;
}

std::vector<std::string> MatchingCodes(const std::set<std::string>& codes,
                                       std::initializer_list<const char*> candidates)
{
    std::vector<std::string> matches;
    for (const char* candidate : candidates) {
        if (codes.find(candidate) != codes.end()) matches.emplace_back(candidate);
    }
    return matches;
}

void AddAction(TraceBridgeRecoveryPlan& plan, RecoveryActionKind kind,
               bool available, bool confirmation, bool changesDut,
               std::string summary, std::vector<std::string> issueCodes)
{
    TraceBridgeRecoveryAction action;
    action.kind = kind;
    action.id = ToString(kind);
    action.available = available;
    action.requiresUserConfirmation = confirmation;
    action.changesDut = changesDut;
    action.summary = std::move(summary);
    action.issueCodes = std::move(issueCodes);
    plan.actions.push_back(std::move(action));
}

} // namespace

const char* ToString(RecoveryActionKind kind)
{
    switch (kind) {
        case RecoveryActionKind::SelectDevice: return "device.select";
        case RecoveryActionKind::ReconnectTransport: return "transport.reconnect";
        case RecoveryActionKind::ReleaseControl: return "control.release";
        case RecoveryActionKind::ReduceProbeCount: return "probe.reduce";
        case RecoveryActionKind::OpenTimingReport: return "timing.open_report";
        case RecoveryActionKind::RearmCapture: return "capture.rearm";
    }
    return "recovery.unknown";
}

bool TraceBridgeRecoveryService::Build(const TraceBridgeRecoveryContext& context,
                                       TraceBridgeRecoveryPlan& plan,
                                       std::string& error)
{
    error.clear();
    plan = TraceBridgeRecoveryPlan();

    std::set<std::string> codes;
    for (const auto& issue : context.issues) {
        if (!issue.code.empty()) codes.insert(issue.code);
    }

    const bool deviceIssue = ContainsAny(
        codes, {"DEVICE_MISSING", "SERIAL_DEVICE_MISSING", "FTDI_DEVICE_MISSING",
                "FTDI_DRIVER_MISMATCH", "JTAG_CHAIN_MISMATCH", "DEVICE_UNAVAILABLE",
                "DEVICE_REENUMERATED"});
    if (deviceIssue) {
        AddAction(plan, RecoveryActionKind::SelectDevice, !context.captureActive,
                  false, false,
                  context.captureActive
                      ? "capture is active; select another device after stopping at a safe boundary"
                      : "refresh the device list and select the intended target",
                  MatchingCodes(codes, {"DEVICE_MISSING", "SERIAL_DEVICE_MISSING",
                                        "FTDI_DEVICE_MISSING", "FTDI_DRIVER_MISMATCH",
                                        "JTAG_CHAIN_MISMATCH", "DEVICE_UNAVAILABLE",
                                        "DEVICE_REENUMERATED"}));
    }

    const bool transportRecoveryIssue = ContainsAny(
        codes, {"JTAG_TAP_ERROR", "JTAG_PROTOCOL_TIMEOUT", "JTAG_SEQUENCE_ERROR",
                "JTAG_STREAM_MISALIGNED", "TRANSPORT_DISCONNECTED"});
    if (transportRecoveryIssue) {
        AddAction(plan, RecoveryActionKind::ReconnectTransport,
                  !context.captureActive, true, false,
                  context.captureActive
                      ? "stop at a safe boundary before reconnecting the transport"
                      : "reconnect the transport, reset protocol state and revalidate identity",
                  MatchingCodes(codes, {"JTAG_TAP_ERROR", "JTAG_PROTOCOL_TIMEOUT",
                                        "JTAG_SEQUENCE_ERROR", "JTAG_STREAM_MISALIGNED",
                                        "TRANSPORT_DISCONNECTED"}));
    }

    const bool controlIssue = ContainsAny(
        codes, {"DEVICE_BUSY", "CONTROL_LEASE_BUSY", "TRANSPORT_BUSY"});
    if (controlIssue) {
        AddAction(plan, RecoveryActionKind::ReleaseControl,
                  !context.captureActive && context.ownsControl,
                  false, false,
                  context.ownsControl
                      ? (context.captureActive
                             ? "stop at a safe boundary before releasing this task's control lease"
                             : "release this task's control lease without terminating other programs")
                      : "this task does not own the control lease; inspect the owning task first",
                  MatchingCodes(codes, {"DEVICE_BUSY", "CONTROL_LEASE_BUSY",
                                        "TRANSPORT_BUSY"}));
    }

    const bool probeIssue = ContainsAny(
        codes, {"PROBE_BUDGET_EXCEEDED", "PROBE_TOO_WIDE", "DUPLICATE_PROBE_ID",
                "CONTRACT_INVALID"});
    if (probeIssue) {
        AddAction(plan, RecoveryActionKind::ReduceProbeCount,
                  !context.captureActive && context.hasProbePlan,
                  false, false,
                  context.hasProbePlan
                      ? (context.captureActive
                             ? "stop at a safe boundary before editing the probe plan"
                             : "open the probe plan and lower width or remove the least useful probes")
                      : "no probe plan is available to edit",
                  MatchingCodes(codes, {"PROBE_BUDGET_EXCEEDED", "PROBE_TOO_WIDE",
                                        "DUPLICATE_PROBE_ID", "CONTRACT_INVALID"}));
    }

    const bool timingIssue = ContainsAny(
        codes, {"TIMING_VIOLATION", "TIMING_MARGIN_LOW", "BUILD_TIMING_FAILED",
                "TIMING_REPORT_MISSING", "BUILD_FINGERPRINT_MISMATCH"});
    if (timingIssue) {
        AddAction(plan, RecoveryActionKind::OpenTimingReport, context.hasTimingReport,
                  false, false,
                  context.hasTimingReport
                      ? "open the timing/resource report and inspect the limiting clock domain"
                      : "timing/resource report is unavailable for this attempt",
                  MatchingCodes(codes, {"TIMING_VIOLATION", "TIMING_MARGIN_LOW",
                                        "BUILD_TIMING_FAILED", "TIMING_REPORT_MISSING",
                                        "BUILD_FINGERPRINT_MISMATCH"}));
    }

    const bool rearmIssue = ContainsAny(
        codes, {"CAPTURE_TIMEOUT", "ARM_FAILED", "DEVICE_RESET_REQUIRED",
                "RETRY_SAFE", "SYNC_LOST"});
    if (rearmIssue) {
        AddAction(plan, RecoveryActionKind::RearmCapture,
                  !context.captureActive && context.deviceAvailable,
                  true, true,
                  context.deviceAvailable
                      ? (context.captureActive
                             ? "stop at a safe boundary before re-arming the DUT"
                             : "confirm before re-arming; this sends a new capture command to the DUT")
                      : "re-arm is unavailable until the target device is reachable",
                  MatchingCodes(codes, {"CAPTURE_TIMEOUT", "ARM_FAILED",
                                        "DEVICE_RESET_REQUIRED", "RETRY_SAFE", "SYNC_LOST"}));
    }

    return true;
}

} // namespace sigflow::debug
