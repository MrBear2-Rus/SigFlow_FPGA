#pragma once

#include "CaptureRunService.h"

#include <string>
#include <vector>

namespace sigflow::debug {

enum class CaptureWizardStage {
    Target,
    Contract,
    Impact,
    Device,
    ArmCapture,
    Results,
};

enum class CaptureWizardStepState {
    Locked,
    Blocked,
    Ready,
    Active,
    Complete,
};

const char* ToString(CaptureWizardStage stage);
const char* ToString(CaptureWizardStepState state);

struct TraceBridgeCaptureWizardInput {
    std::string targetLabel;
    bool targetReady = false;
    bool contractReady = false;
    bool impactReportReady = false;
    bool deviceReady = false;
    bool captureActive = false;
    bool captureComplete = false;
    bool resultReady = false;
    std::vector<DiagnosticIssue> issues;
};

struct TraceBridgeCaptureWizardStep {
    std::string id;
    CaptureWizardStage stage = CaptureWizardStage::Target;
    CaptureWizardStepState state = CaptureWizardStepState::Locked;
    bool canEnter = false;
    bool requiresUserAction = false;
    std::string navigationTarget;
    std::string summary;
    std::vector<DiagnosticIssue> issues;
};

struct TraceBridgeCaptureWizardPlan {
    CaptureWizardStage currentStage = CaptureWizardStage::Target;
    bool readyToStart = false;
    bool complete = false;
    std::vector<TraceBridgeCaptureWizardStep> steps;
    std::vector<DiagnosticIssue> issues;
};

// 首次采集向导的宿主模型。它只编排既有配置/预检/采集入口，不复制契约参数，
// 也不自动打开设备、布防或下载；GUI、CLI 和后续自动化都可消费同一份计划。
class TraceBridgeCaptureWizardService {
public:
    static bool Build(const TraceBridgeCaptureWizardInput& input,
                      TraceBridgeCaptureWizardPlan& plan,
                      std::string& error);
};

} // namespace sigflow::debug
