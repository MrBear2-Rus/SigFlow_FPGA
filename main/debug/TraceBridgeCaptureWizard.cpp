#include "TraceBridgeCaptureWizard.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace sigflow::debug {
namespace {

struct StepSpec {
    const char* id;
    CaptureWizardStage stage;
    const char* navigationTarget;
};

constexpr StepSpec kSteps[] = {
    {"target.tool", CaptureWizardStage::Target, "tracebridge.target"},
    {"contract.probes", CaptureWizardStage::Contract, "tracebridge.contract"},
    {"impact.report", CaptureWizardStage::Impact, "tracebridge.impact"},
    {"device.preflight", CaptureWizardStage::Device, "tracebridge.device"},
    {"capture.arm", CaptureWizardStage::ArmCapture, "tracebridge.capture"},
    {"results.waveform", CaptureWizardStage::Results, "tracebridge.results"},
};

DiagnosticIssue Issue(const std::string& code, const std::string& observed,
                      const std::string& action)
{
    DiagnosticIssue issue;
    issue.code = code;
    issue.stage = "wizard";
    issue.severity = DiagnosticSeverity::Blocking;
    issue.observed = observed;
    issue.action = action;
    return issue;
}

bool Prefix(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

bool BelongsTo(CaptureWizardStage stage, const std::string& code)
{
    switch (stage) {
        case CaptureWizardStage::Target:
            return code == "TARGET_NOT_SELECTED" || code == "TOOLCHAIN_NOT_SELECTED" ||
                   code == "BUILD_FINGERPRINT_MISMATCH";
        case CaptureWizardStage::Contract:
            return Prefix(code, "CONTRACT_") || Prefix(code, "PROBE_") ||
                   code == "DUPLICATE_PROBE_ID";
        case CaptureWizardStage::Impact:
            return Prefix(code, "IMPACT_") || Prefix(code, "TIMING_") ||
                   code == "BUILD_TIMING_FAILED";
        case CaptureWizardStage::Device:
            return Prefix(code, "DEVICE_") || Prefix(code, "SERIAL_") ||
                   Prefix(code, "FTDI_") || Prefix(code, "TRANSPORT_");
        case CaptureWizardStage::ArmCapture:
            return Prefix(code, "CAPTURE_") || Prefix(code, "ARM_") ||
                   Prefix(code, "SYNC_");
        case CaptureWizardStage::Results:
            return Prefix(code, "RESULT_") || Prefix(code, "PROTOCOL_") ||
                   Prefix(code, "SAVE_") || Prefix(code, "VCD_");
    }
    return false;
}

void AddIssue(TraceBridgeCaptureWizardStep& step, TraceBridgeCaptureWizardPlan& plan,
              const DiagnosticIssue& issue)
{
    step.issues.push_back(issue);
    plan.issues.push_back(issue);
}

} // namespace

const char* ToString(CaptureWizardStage stage)
{
    switch (stage) {
        case CaptureWizardStage::Target: return "target";
        case CaptureWizardStage::Contract: return "contract";
        case CaptureWizardStage::Impact: return "impact";
        case CaptureWizardStage::Device: return "device";
        case CaptureWizardStage::ArmCapture: return "arm_capture";
        case CaptureWizardStage::Results: return "results";
    }
    return "target";
}

const char* ToString(CaptureWizardStepState state)
{
    switch (state) {
        case CaptureWizardStepState::Locked: return "locked";
        case CaptureWizardStepState::Blocked: return "blocked";
        case CaptureWizardStepState::Ready: return "ready";
        case CaptureWizardStepState::Active: return "active";
        case CaptureWizardStepState::Complete: return "complete";
    }
    return "locked";
}

bool TraceBridgeCaptureWizardService::Build(const TraceBridgeCaptureWizardInput& input,
                                            TraceBridgeCaptureWizardPlan& plan,
                                            std::string& error)
{
    error.clear();
    plan = TraceBridgeCaptureWizardPlan();
    if (input.targetReady && input.targetLabel.empty()) {
        error = "wizard target is marked ready but has no target label";
        plan.issues.push_back(Issue("TARGET_LABEL_MISSING", error,
                                    "select the board/tool target before continuing"));
        return false;
    }

    const bool prerequisites[] = {
        input.targetReady,
        input.contractReady,
        input.impactReportReady,
        input.deviceReady,
    };
    const bool completed[] = {
        input.targetReady,
        input.contractReady,
        input.impactReportReady,
        input.deviceReady,
        input.captureComplete,
        input.resultReady,
    };

    bool previousReady = true;
    for (std::size_t index = 0; index < std::size(kSteps); ++index) {
        const StepSpec& spec = kSteps[index];
        TraceBridgeCaptureWizardStep step;
        step.id = spec.id;
        step.stage = spec.stage;
        step.navigationTarget = spec.navigationTarget;
        step.canEnter = previousReady;

        if (index < 4 && !prerequisites[index]) {
            step.state = previousReady ? CaptureWizardStepState::Blocked
                                       : CaptureWizardStepState::Locked;
            step.requiresUserAction = true;
            if (index == 0) {
                AddIssue(step, plan, Issue("TARGET_NOT_SELECTED",
                                           "no target/toolchain is ready",
                                           "select the intended board and toolchain"));
            } else if (index == 1) {
                AddIssue(step, plan, Issue("CONTRACT_NOT_READY",
                                           "debug contract or probe plan is not ready",
                                           "open the existing contract/probe configuration"));
            } else if (index == 2) {
                AddIssue(step, plan, Issue("IMPACT_REPORT_MISSING",
                                           "instrumentation impact report is not ready",
                                           "review the probe/resource impact report"));
            } else {
                AddIssue(step, plan, Issue("DEVICE_PREFLIGHT_REQUIRED",
                                           "device preflight has not passed",
                                           "select the device and run the read-only preflight"));
            }
            step.summary = "complete the prerequisite before entering this step";
        } else if (spec.stage == CaptureWizardStage::ArmCapture) {
            step.requiresUserAction = true;
            if (input.captureActive) {
                step.state = CaptureWizardStepState::Active;
                step.canEnter = true;
                step.summary = "capture is active; wait or cancel at a safe boundary";
            } else if (input.captureComplete) {
                step.state = CaptureWizardStepState::Complete;
                step.summary = "capture completed; inspect the generated result";
            } else {
                step.state = previousReady ? CaptureWizardStepState::Ready
                                            : CaptureWizardStepState::Locked;
                step.summary = previousReady
                                   ? "ready to arm and capture after explicit user action"
                                   : "complete the preceding preflight steps first";
            }
        } else if (spec.stage == CaptureWizardStage::Results) {
            step.state = input.resultReady ? CaptureWizardStepState::Complete
                                           : (previousReady ? CaptureWizardStepState::Ready
                                                             : CaptureWizardStepState::Locked);
            step.canEnter = previousReady;
            step.summary = input.resultReady
                               ? "waveform/protocol results are available"
                               : "open the result view after capture artifacts are ready";
        } else {
            step.state = CaptureWizardStepState::Complete;
            step.summary = "prerequisite completed; configuration remains in its existing panel";
        }

        for (const auto& issue : input.issues) {
            if (BelongsTo(spec.stage, issue.code)) AddIssue(step, plan, issue);
        }
        if (!step.issues.empty() && index < 4 &&
            step.state != CaptureWizardStepState::Locked) {
            step.state = CaptureWizardStepState::Blocked;
            step.canEnter = false;
            step.requiresUserAction = true;
        }
        if (step.state == CaptureWizardStepState::Blocked ||
            step.state == CaptureWizardStepState::Locked) {
            previousReady = false;
        } else if (index < 4) {
            previousReady = true;
        }
        plan.steps.push_back(std::move(step));
    }

    plan.readyToStart = input.targetReady && input.contractReady &&
                        input.impactReportReady && input.deviceReady &&
                        !input.captureActive;
    plan.complete = input.resultReady;
    for (const auto& step : plan.steps) {
        if (step.state == CaptureWizardStepState::Active ||
            step.state == CaptureWizardStepState::Ready ||
            step.state == CaptureWizardStepState::Blocked) {
            plan.currentStage = step.stage;
            break;
        }
        plan.currentStage = step.stage;
    }
    if (plan.complete) plan.currentStage = CaptureWizardStage::Results;
    return true;
}

} // namespace sigflow::debug
